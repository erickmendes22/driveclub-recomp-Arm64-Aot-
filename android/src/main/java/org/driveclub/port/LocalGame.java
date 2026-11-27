package org.driveclub.port;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.stream.Stream;

/** Resolve Android document trees into the private inputs of the shared PC preparer. */
final class LocalGame {
    private static native String prepareNative(String root);

    private static Uri find(GameLibrary library, Uri tree, String relative) throws IOException {
        String parent = DocumentsContract.getTreeDocumentId(tree);
        int slash = relative.indexOf('/');
        if (slash >= 0) {
            Uri dir = library.child(tree, parent, relative.substring(0, slash), true);
            if (dir == null) return null;
            parent = DocumentsContract.getDocumentId(dir);
        }
        return library.child(tree, parent, relative.substring(slash + 1), false);
    }

    private static void copy(Context context, Uri file, Path dest) throws IOException {
        if (file == null) throw new IOException("Arquivo ausente: " + dest.getFileName());
        Files.createDirectories(dest.getParent());
        try (InputStream in = context.getContentResolver().openInputStream(file)) {
            if (in == null) throw new IOException("Falha de leitura: " + dest.getFileName());
            Files.copy(in, dest);
        }
    }

    private static void remove(Path path) throws IOException {
        if (!Files.exists(path)) return;
        try (Stream<Path> entries = Files.walk(path)) {
            for (Path item : (Iterable<Path>) entries.sorted(Comparator.reverseOrder())::iterator)
                Files.delete(item);
        }
    }

    static synchronized void prepare(Context context, GameLibrary library, Uri base, Uri update,
                                     boolean refresh, PortLog log) throws IOException {
        Path root = context.getNoBackupFilesDir().toPath().resolve("driveclub");
        Files.createDirectories(root);
        SharedPreferences state = context.getSharedPreferences("native-runtime", Context.MODE_PRIVATE);
        String active = state.getString("active", "");
        if (!refresh && !active.isEmpty() && base.toString().equals(state.getString("base", ""))
                && update.toString().equals(state.getString("update", ""))
                && Files.isRegularFile(root.resolve(active).resolve("work/prep/boot-linked.bin"))) return;
        // Remove interrupted staging directories; preserve the last complete generation on failure.
        try (Stream<Path> children = Files.list(root)) {
            for (Path path : (Iterable<Path>) children::iterator)
                if (!path.getFileName().toString().equals(active)) remove(path);
        }
        Uri data = find(library, base, "game000.dat");
        if (data == null) throw new IOException("game000.dat ausente na pasta base");
        Path next = Files.createTempDirectory(root, "local-");
        boolean published = false;
        try (ParcelFileDescriptor fd = context.getContentResolver().openFileDescriptor(data, "r")) {
            if (fd == null) throw new IOException("game000.dat inacessível");
            log.write("Preparação local", "Lendo executável e módulos no aparelho");
            Path updateDir = next.resolve("update");
            Path baseDir = Files.createDirectories(next.resolve("base"));
            // PrepareGame checks the base archive exists. Keep its real descriptor alive
            // through the call so that even very large archives stay in the selected tree.
            Files.createSymbolicLink(baseDir.resolve("game000.dat"), new File("/proc/self/fd/" + fd.getFd()).toPath());
            copy(context, find(library, update, "eboot.bin"), updateDir.resolve("eboot.bin"));
            Uri sfo = find(library, update, "sce_sys/param.sfo");
            copy(context, sfo != null ? sfo : find(library, update, "param.sfo"), updateDir.resolve("sce_sys/param.sfo"));
            for (String name : new String[]{"sce_module/libc.prx", "sce_module/libSceFios2.prx", "libtbb.prx",
                    "libfmod.prx", "libfmodstudio.prx", "iksemelGNMm.prx", "logiWheel.prx", "oskGNMm.prx"}) {
                boolean baseFirst = name.startsWith("libfmod");
                Uri module = find(library, baseFirst ? base : update, name);
                if (module == null) module = find(library, baseFirst ? update : base, name);
                if (module != null || !name.equals("oskGNMm.prx")) copy(context, module, updateDir.resolve(name));
            }
            try (InputStream xml = context.getAssets().open("DriveClub.xml")) {
                Files.copy(xml, next.resolve("patches.xml"));
            }
            String error = prepareNative(next.toString());
            if (!error.isEmpty()) throw new IOException(error);
            remove(baseDir);
            remove(updateDir);
            Files.delete(next.resolve("patches.xml"));
            if (!base.equals(library.folder(GameLibrary.BASE)) || !update.equals(library.folder(GameLibrary.UPDATE))) return;
            published = state.edit().putString("active", next.getFileName().toString())
                    .putString("base", base.toString()).putString("update", update.toString()).commit();
            if (!published) throw new IOException("Falha ao salvar estado local");
            log.write("Preparação local", "Módulos vinculados e patches compilados no aparelho");
            if (!active.isEmpty()) remove(root.resolve(active));
        } finally {
            if (!published) remove(next);
        }
    }
}
