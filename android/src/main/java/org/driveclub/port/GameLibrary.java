package org.driveclub.port;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.Objects;
import java.util.HashSet;
import java.util.Set;
import java.util.stream.Stream;

/** A game folder can already include its update; a separate update is an optional overlay. */
final class GameLibrary {
    static final String BASE = "base", UPDATE = "update";
    private static final String PREVIOUS_BASE = "previous-base", PREVIOUS_UPDATE = "previous-update", CLEANUP = "pending-cleanup";
    private final Context context;
    private final SharedPreferences prefs;

    GameLibrary(Context context) {
        this.context = context.getApplicationContext();
        prefs = context.getSharedPreferences("game-library", Context.MODE_PRIVATE);
    }

    Uri folder(String key) {
        String value = prefs.getString(key, null);
        return value == null ? null : Uri.parse(value);
    }

    Uri effectiveUpdate() {
        Uri update = folder(UPDATE);
        return update == null ? folder(BASE) : update;
    }

    boolean matches(Uri base, Uri update) {
        return Objects.equals(base, folder(BASE)) && Objects.equals(update, effectiveUpdate());
    }

    void select(String key, Uri tree) throws IOException {
        if (!"content".equals(tree.getScheme()) || !android.provider.DocumentsContract.isTreeUri(tree))
            throw new IOException("Selecione uma pasta pelo seletor do Android");
        context.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        Uri oldBase = folder(BASE), oldUpdate = folder(UPDATE);
        SharedPreferences.Editor change = prefs.edit().putString(key, tree.toString());
        rememberSelection(change, oldBase, oldUpdate);
        // A newly selected game is self-contained unless the user adds an overlay afterwards.
        if (BASE.equals(key)) change.remove(UPDATE);
        if (!change.commit()) throw new IOException("Não foi possível salvar a pasta selecionada");
        releaseUnusedGrant(oldBase);
        releaseUnusedGrant(oldUpdate);
    }

    void clearUpdate() throws IOException {
        Uri old = folder(UPDATE);
        SharedPreferences.Editor change = prefs.edit().remove(UPDATE);
        rememberSelection(change, folder(BASE), old);
        if (!change.commit()) throw new IOException("Falha ao alterar a atualização");
        releaseUnusedGrant(old);
    }

    private void releaseUnusedGrant(Uri old) {
        if (old == null || !"content".equals(old.getScheme()) || old.equals(folder(BASE)) || old.equals(folder(UPDATE))
                || old.equals(folder(PREVIOUS_BASE)) || old.equals(folder(PREVIOUS_UPDATE))) return;
        try {
            context.getContentResolver().releasePersistableUriPermission(old, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) { /* The provider may already have revoked it. */ }
    }

    /** Publish only a complete extraction. No current files or save data are modified in place. */
    void publish(PkgInstaller.Result result, PortLog log) throws IOException {
        if (!"CUSA00093".equals(result.titleId)) throw new IOException("Este PKG não pertence ao Driveclub CUSA00093");
        Uri uri = Uri.fromFile(result.root.toFile());
        RuntimeFiles.checkedInstalledRoot(context, uri);
        Uri oldBase = folder(BASE), oldUpdate = folder(UPDATE);
        if (result.isUpdate && oldBase == null)
            throw new IOException("Instale o jogo base ou selecione sua pasta antes de instalar a atualização");
        SharedPreferences.Editor change = prefs.edit();
        if (result.isUpdate) change.putString(UPDATE, uri.toString());
        else change.putString(BASE, uri.toString()).remove(UPDATE);
        rememberSelection(change, oldBase, oldUpdate);
        if (!change.commit()) throw new IOException("Não foi possível ativar o PKG instalado");
        releaseUnusedGrant(oldBase);
        releaseUnusedGrant(oldUpdate);
    }

    private void rememberSelection(SharedPreferences.Editor change, Uri oldBase, Uri oldUpdate) {
        Set<String> cleanup = new HashSet<>(prefs.getStringSet(CLEANUP, java.util.Collections.emptySet()));
        remember(cleanup, oldBase);
        remember(cleanup, oldUpdate);
        change.putStringSet(CLEANUP, cleanup);
        if (folder(PREVIOUS_BASE) == null && oldBase != null) {
            change.putString(PREVIOUS_BASE, oldBase.toString());
            if (oldUpdate == null) change.remove(PREVIOUS_UPDATE);
            else change.putString(PREVIOUS_UPDATE, oldUpdate.toString());
        }
    }

    private static void remember(Set<String> cleanup, Uri uri) {
        if (uri != null && "file".equals(uri.getScheme())) cleanup.add(uri.toString());
    }

    boolean canRestorePrevious() { return folder(PREVIOUS_BASE) != null; }

    void restorePrevious() throws IOException {
        Uri base = folder(PREVIOUS_BASE), update = folder(PREVIOUS_UPDATE);
        if (base == null) throw new IOException("Não há instalação anterior para restaurar");
        Set<String> cleanup = new HashSet<>(prefs.getStringSet(CLEANUP, java.util.Collections.emptySet()));
        Uri currentBase = folder(BASE), currentUpdate = folder(UPDATE);
        remember(cleanup, currentBase);
        remember(cleanup, currentUpdate);
        SharedPreferences.Editor change = prefs.edit().putString(BASE, base.toString()).putStringSet(CLEANUP, cleanup)
                .remove(PREVIOUS_BASE).remove(PREVIOUS_UPDATE);
        if (update == null) change.remove(UPDATE); else change.putString(UPDATE, update.toString());
        if (!change.commit()) throw new IOException("Não foi possível restaurar a seleção anterior");
        releaseUnusedGrant(currentBase);
        releaseUnusedGrant(currentUpdate);
    }

    /** Old package generations are reclaimed only after the new game preparation has succeeded. */
    void finishPreparation(PortLog log) {
        Uri previousBase = folder(PREVIOUS_BASE), previousUpdate = folder(PREVIOUS_UPDATE);
        if (!prefs.edit().remove(PREVIOUS_BASE).remove(PREVIOUS_UPDATE).commit()) {
            log.write("PKG", "Preparação concluída; a limpeza da instalação anterior ficará para depois");
            return;
        }
        Set<String> failed = new HashSet<>();
        for (String uri : new HashSet<>(prefs.getStringSet(CLEANUP, java.util.Collections.emptySet())))
            if (!removeUnusedInstallation(Uri.parse(uri), log)) failed.add(uri);
        prefs.edit().putStringSet(CLEANUP, failed).apply();
        releaseUnusedGrant(previousBase);
        releaseUnusedGrant(previousUpdate);
    }

    private boolean removeUnusedInstallation(Uri previous, PortLog log) {
        if (previous == null || !"file".equals(previous.getScheme())
                || previous.equals(folder(BASE)) || previous.equals(folder(UPDATE))) return true;
        try {
            if (previous.getPath() != null && !Files.exists(new java.io.File(previous.getPath()).toPath())) return true;
            Path root = RuntimeFiles.checkedInstalledRoot(context, previous);
            try (Stream<Path> files = Files.walk(root)) {
                for (Path path : (Iterable<Path>) files.sorted(Comparator.reverseOrder())::iterator) Files.delete(path);
            }
            return true;
        } catch (IOException | java.io.UncheckedIOException | SecurityException e) {
            log.write("PKG", "Instalação ativada; não foi possível limpar a anterior: " + e.getMessage());
            return false;
        }
    }

    void discard(PkgInstaller.Result result, PortLog log) {
        if (result != null) removeUnusedInstallation(Uri.fromFile(result.root.toFile()), log);
    }

    String summary(String key) throws IOException {
        Uri tree = folder(key);
        if (tree == null) return BASE.equals(key)
                ? "Escolha a pasta com game000.dat. Ela pode já conter a atualização 1.28."
                : "Opcional: use apenas se a atualização estiver em outra pasta.";
        try (RuntimeFiles source = new RuntimeFiles(context, tree, tree)) {
            String prefix = "file".equals(tree.getScheme()) ? "Instalado por PKG.\n" : "";
            if (BASE.equals(key) && !source.hasFile("game000.dat"))
                return prefix + "game000.dat ausente na pasta selecionada.";
            String sfo = source.hasFile("sce_sys/param.sfo") ? "sce_sys/param.sfo"
                    : source.hasFile("param.sfo") ? "param.sfo" : null;
            if (sfo == null) return prefix + (BASE.equals(key)
                    ? "game000.dat encontrado. Adicione a atualização 1.28 se ela estiver em outra pasta."
                    : "param.sfo ausente na atualização selecionada.");
            try (InputStream input = source.openInput(sfo)) {
                return prefix + (BASE.equals(key) ? "game000.dat encontrado.\n" : "") + inspectStream(input);
            }
        }
    }

    String inspectFile(Uri uri) throws IOException {
        try (InputStream input = context.getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException("Arquivo inacessível");
            return inspectStream(input);
        }
    }

    private String inspectStream(InputStream input) throws IOException {
        if (!PortActivity.nativeAvailable()) return "Biblioteca nativa indisponível. Consulte os logs.";
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] block = new byte[4096];
        int count;
        while ((count = input.read(block)) != -1) {
            if (bytes.size() + count > 65536) throw new IOException("param.sfo excede o limite de 64 KiB");
            bytes.write(block, 0, count);
        }
        return PortActivity.inspect(bytes.toByteArray());
    }
}
