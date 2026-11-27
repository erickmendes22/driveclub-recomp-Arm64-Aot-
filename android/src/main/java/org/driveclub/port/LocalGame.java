package org.driveclub.port;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.stream.Stream;

/** Prepare selected folders or installed packages entirely on this device. */
final class LocalGame {
    interface Progress { void update(int stage, String message); }
    private static native String prepareNative(String root, Progress progress);
    private static final String[] MODULES = {"sce_module/libc.prx", "sce_module/libSceFios2.prx", "libtbb.prx",
            "libfmod.prx", "libfmodstudio.prx", "iksemelGNMm.prx", "logiWheel.prx", "oskGNMm.prx"};

    private static void copy(RuntimeFiles files, String relative, Path dest) throws IOException {
        Files.createDirectories(dest.getParent());
        try (InputStream in = files.openInput(relative)) {
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

    private static String sourceStamp(Context context, Uri base, Uri update) throws IOException {
        StringBuilder stamp = new StringBuilder().append(BuildConfig.VERSION_CODE).append('\n')
                .append(base).append('\n').append(update);
        try (RuntimeFiles baseFiles = new RuntimeFiles(context, base, base);
             RuntimeFiles updateFiles = new RuntimeFiles(context, update, update)) {
            String[] inputs = {"game000.dat", "eboot.bin", "sce_sys/param.sfo", "param.sfo"};
            for (String name : inputs) {
                stamp.append('\n').append(name).append(':').append(java.util.Arrays.toString(baseFiles.statFile(name)))
                        .append(':').append(java.util.Arrays.toString(updateFiles.statFile(name)));
            }
            for (String name : MODULES) {
                stamp.append('\n').append(name).append(':').append(java.util.Arrays.toString(baseFiles.statFile(name)))
                        .append(':').append(java.util.Arrays.toString(updateFiles.statFile(name)));
            }
        }
        return stamp.toString();
    }

    private static boolean complete(Path generation) {
        for (String name : new String[]{"boot-linked.bin", "content.bin", "patches.bin"})
            if (!Files.isRegularFile(generation.resolve("work/prep").resolve(name))) return false;
        return Files.isRegularFile(generation.resolve("update/sce_sys/param.sfo"));
    }

    static synchronized Path prepare(Context context, GameLibrary library, Uri base, Uri update,
                                     boolean refresh, PortLog log, Progress progress) throws IOException {
        Path root = context.getNoBackupFilesDir().toPath().resolve("driveclub");
        Files.createDirectories(root);
        SharedPreferences state = context.getSharedPreferences("native-runtime", Context.MODE_PRIVATE);
        String active = state.getString("active", "");
        String stamp = sourceStamp(context, base, update);
        if (!refresh && !active.isEmpty() && base.toString().equals(state.getString("base", ""))
                && update.toString().equals(state.getString("update", ""))
                && stamp.equals(state.getString("sourceStamp", "")) && complete(root.resolve(active))) {
            log.write("Preparação local", "Reutilizando arquivos preparados neste aparelho");
            return root.resolve(active);
        }
        // Remove interrupted staging directories; preserve the last complete generation on failure.
        try (Stream<Path> children = Files.list(root)) {
            for (Path path : (Iterable<Path>) children::iterator)
                if (path.getFileName().toString().startsWith("local-")
                        && !path.getFileName().toString().equals(active)) remove(path);
        }
        Path next = Files.createTempDirectory(root, "local-");
        boolean published = false;
        try (RuntimeFiles baseFiles = new RuntimeFiles(context, base, base);
             RuntimeFiles updateFiles = new RuntimeFiles(context, update, update);
             ParcelFileDescriptor fd = baseFiles.openDescriptor("game000.dat")) {
            log.write("Preparação local", "Lendo executável e módulos no aparelho");
            Path updateDir = next.resolve("update");
            Path baseDir = Files.createDirectories(next.resolve("base"));
            // PrepareGame checks the base archive exists. Keep its real descriptor alive
            // through the call so that even very large archives stay in the selected tree.
            Files.createSymbolicLink(baseDir.resolve("game000.dat"), new File("/proc/self/fd/" + fd.getFd()).toPath());
            copy(updateFiles, "eboot.bin", updateDir.resolve("eboot.bin"));
            copy(updateFiles, updateFiles.hasFile("sce_sys/param.sfo") ? "sce_sys/param.sfo" : "param.sfo",
                    updateDir.resolve("sce_sys/param.sfo"));
            for (String name : MODULES) {
                boolean baseFirst = name.startsWith("libfmod");
                RuntimeFiles first = baseFirst ? baseFiles : updateFiles;
                RuntimeFiles second = baseFirst ? updateFiles : baseFiles;
                RuntimeFiles source = first.hasFile(name) ? first : second.hasFile(name) ? second : null;
                if (source != null) copy(source, name, updateDir.resolve(name));
                else if (!name.equals("oskGNMm.prx")) throw new IOException("Arquivo ausente: " + name);
            }
            try (InputStream xml = context.getAssets().open("DriveClub.xml")) {
                Files.copy(xml, next.resolve("patches.xml"));
            }
            String error = prepareNative(next.toString(), progress);
            if (!error.isEmpty()) throw new IOException(error);
            remove(baseDir);
            // The loader reads this small metadata file directly at startup. Game
            // content remains in the selected document trees for the runtime bridge.
            try (Stream<Path> inputs = Files.list(updateDir)) {
                for (Path item : (Iterable<Path>) inputs::iterator)
                    if (!item.getFileName().toString().equals("sce_sys")) remove(item);
            }
            Files.delete(next.resolve("patches.xml"));
            if (!complete(next)) throw new IOException("A preparação local não gerou todos os arquivos necessários");
            if (!library.matches(base, update)) return null;
            published = state.edit().putString("active", next.getFileName().toString())
                    .putString("base", base.toString()).putString("update", update.toString())
                    .putString("sourceStamp", stamp).commit();
            if (!published) throw new IOException("Falha ao salvar estado local");
            log.write("Preparação local", "Módulos vinculados e patches compilados no aparelho");
            if (!active.isEmpty()) remove(root.resolve(active));
            return next;
        } finally {
            if (!published) remove(next);
        }
    }
}
