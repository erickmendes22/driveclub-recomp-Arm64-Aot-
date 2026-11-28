package org.driveclub.port;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.IOException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.UUID;
import org.json.JSONException;
import org.json.JSONObject;

/** Local PS4 FPKG extraction. Publication of the returned generation belongs to GameLibrary. */
final class PkgInstaller {
    interface Progress {
        /** Called on the installation worker; return false to cancel. */
        boolean update(long done, long total, String path);
    }

    static final class Plan {
        final String titleId, title, version, contentId, fingerprint;
        final boolean isUpdate;
        final long packageBytes, extractedBytes;

        private Plan(JSONObject value) throws JSONException {
            titleId = value.getString("titleId");
            title = value.getString("title");
            version = value.getString("version");
            contentId = value.getString("contentId");
            fingerprint = value.getString("fingerprint");
            isUpdate = value.getBoolean("isUpdate");
            packageBytes = value.getLong("packageBytes");
            extractedBytes = value.getLong("extractedBytes");
        }
    }

    static final class Result {
        final String titleId, version;
        final boolean isUpdate;
        final Path root;
        final long extractedBytes;

        private Result(Plan plan, Path root) {
            titleId = plan.titleId;
            version = plan.version;
            isUpdate = plan.isUpdate;
            extractedBytes = plan.extractedBytes;
            this.root = root;
        }
    }

    static final class CancelledException extends IOException {
        CancelledException() { super("Instalação cancelada. A instalação anterior foi preservada."); }
    }

    private static boolean loaded;

    private static synchronized void load() throws IOException {
        if (loaded) return;
        try {
            System.loadLibrary("driveclub_pkg");
            loaded = true;
        } catch (LinkageError error) {
            throw new IOException("O extrator PKG nativo não está disponível nesta versão/arquitetura do aplicativo.", error);
        }
    }

    private static ParcelFileDescriptor open(Context context, Uri source) throws IOException {
        if (source == null) throw new IOException("Selecione um arquivo PKG.");
        ParcelFileDescriptor fd = context.getContentResolver().openFileDescriptor(source, "r");
        if (fd == null) throw new IOException("Não foi possível abrir o PKG selecionado.");
        return fd;
    }

    private static Plan response(String encoded) throws IOException {
        try {
            if (encoded == null) throw new IOException("O extrator PKG não retornou um resultado.");
            JSONObject value = new JSONObject(encoded);
            if (!value.optBoolean("ok")) {
                if (value.optBoolean("cancelled")) throw new CancelledException();
                throw new IOException(value.optString("error", "Não foi possível extrair o PKG."));
            }
            return new Plan(value.getJSONObject("plan"));
        } catch (JSONException error) {
            throw new IOException("Resposta inválida do extrator PKG.", error);
        }
    }

    static Plan inspect(Context context, Uri source) throws IOException {
        load();
        try (ParcelFileDescriptor fd = open(context, source)) {
            return response(inspectNative(fd.getFd()));
        }
    }

    static synchronized Result install(Context context, Uri source, Plan expected, Progress progress) throws IOException {
        if (expected == null) throw new IOException("Inspecione o PKG antes de instalar.");
        load();
        Path installed = context.getNoBackupFilesDir().toPath().resolve("driveclub/installed");
        Path staging = null;
        boolean complete = false;
        Throwable failure = null;
        try {
            Plan extracted;
            try (ParcelFileDescriptor fd = open(context, source)) {
                Plan current = response(inspectNative(fd.getFd()));
                if (!current.fingerprint.equals(expected.fingerprint))
                    throw new IOException("O PKG mudou desde a seleção. Selecione-o novamente.");
                if (progress != null && !progress.update(0, current.extractedBytes, "Verificando PKG"))
                    throw new CancelledException();
                Files.createDirectories(installed);
                cleanInterrupted(installed);
                staging = Files.createTempDirectory(installed, ".pkg-staging-");
                extracted = response(extractNative(fd.getFd(), staging.toString(), current.fingerprint, progress));
                if (!extracted.fingerprint.equals(current.fingerprint))
                    throw new IOException("Os metadados do PKG mudaram durante a instalação.");
            }
            Path destination = installed.resolve((extracted.isUpdate ? "update-" : "base-") + UUID.randomUUID());
            try {
                Files.move(staging, destination, StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException error) {
                throw new IOException("O armazenamento privado não permite concluir a instalação atomicamente.", error);
            }
            complete = true;
            return new Result(extracted, destination);
        } catch (IOException | RuntimeException | Error error) {
            failure = error;
            throw error;
        } finally {
            if (!complete && staging != null) {
                try { deleteStaging(staging); }
                catch (IOException cleanup) {
                    if (failure != null) failure.addSuppressed(cleanup);
                    else throw cleanup;
                }
            }
        }
    }

    private static void cleanInterrupted(Path installed) throws IOException {
        // Installation is serialized in this process. These private temporary
        // roots can only remain after an interrupted earlier process; published
        // base-/update- roots are deliberately outside this cleanup pattern.
        try (java.nio.file.DirectoryStream<Path> children = Files.newDirectoryStream(installed, ".pkg-staging-*")) {
            for (Path child : children) deleteStaging(child);
        }
    }

    private static void deleteStaging(Path staging) throws IOException {
        // This walk never follows symlinks and only visits this invocation's unique staging root.
        Files.walkFileTree(staging, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                Files.delete(file);
                return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path directory, IOException failure) throws IOException {
                if (failure != null) throw failure;
                Files.delete(directory);
                return FileVisitResult.CONTINUE;
            }
        });
    }

    private static native String inspectNative(int fd);
    private static native String extractNative(int fd, String staging, String fingerprint, Progress progress);
}
