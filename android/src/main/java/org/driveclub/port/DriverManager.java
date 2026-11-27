package org.driveclub.port;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Comparator;
import java.util.stream.Stream;
import java.util.zip.ZipFile;
import org.json.JSONException;
import org.json.JSONObject;

/** Bundled, pinned Turnip selection. It changes this app's Vulkan loader only. */
final class DriverManager {
    static final String TURNIP = "turnip-v39-patched", SYSTEM = "system";
    static final String TURNIP_LABEL = "Turnip Gen8 V39-patched";
    private static final String ASSET = "drivers/turnip-gen8-v39-patched/driver.zip";
    private static final String LIBRARY = "libvulkan_freedreno.so";
    private static final String ZIP_SHA = "bd457b30f195cf93a334b5302c221b1b5cb59c79c485935decf95d560a38c8a2";
    private static final String LIBRARY_SHA = "2db3c4601bc3f046596f86d9d189cb5cad693812f8c8c55fe1817ec90ea879ac";

    private static native String configureNative(String nativeLibraryDir, String driverDir,
                                                  String libraryName, String temporaryDir);

    static String selected(Context context) {
        String fallback = BuildConfig.GAME_RUNTIME ? TURNIP : SYSTEM;
        String value = context.getSharedPreferences("graphics-driver", Context.MODE_PRIVATE).getString("selected", fallback);
        return SYSTEM.equals(value) ? SYSTEM : TURNIP;
    }

    static String label(String choice) { return SYSTEM.equals(choice) ? "Driver do sistema" : TURNIP_LABEL; }

    static void select(Context context, String choice) throws IOException {
        if (!TURNIP.equals(choice) && !SYSTEM.equals(choice)) throw new IOException("Driver desconhecido");
        if (TURNIP.equals(choice) && !BuildConfig.GAME_RUNTIME)
            throw new IOException("O Turnip incluído requer a compilação ARM64");
        if (!context.getSharedPreferences("graphics-driver", Context.MODE_PRIVATE).edit().putString("selected", choice).commit())
            throw new IOException("Não foi possível salvar o driver escolhido");
    }

    static void configure(Context context, PortLog log) throws IOException {
        configure(context, log, selected(context));
    }

    static synchronized void configure(Context context, PortLog log, String choice) throws IOException {
        String directory = "", library = "";
        if (!SYSTEM.equals(choice)) {
            if (!BuildConfig.GAME_RUNTIME) throw new IOException("Turnip disponível apenas em ARM64");
            directory = bundledDirectory(context, log).toString() + "/";
            library = LIBRARY;
        }
        Path temporary = Files.createDirectories(context.getCodeCacheDir().toPath()
                .resolve("driveclub-vulkan").resolve(Integer.toString(android.os.Process.myPid())));
        log.write("Driver", "Selecionado: " + label(choice));
        String error = configureNative(context.getApplicationInfo().nativeLibraryDir + "/",
                directory, library, temporary.toString() + "/");
        if (error == null || !error.isEmpty()) {
            String message = error == null ? "Falha ao configurar o driver Vulkan" : error;
            log.write("Driver", message);
            throw new IOException(message);
        }
        log.write("Driver", "Carregador configurado: " + label(choice)
                + (SYSTEM.equals(choice) ? "" : " · SHA-256 " + LIBRARY_SHA));
    }

    static synchronized Path bundledDirectory(Context context, PortLog log) throws IOException {
        Path parent = Files.createDirectories(context.getNoBackupFilesDir().toPath().resolve("graphics-drivers"));
        Path target = parent.resolve("turnip-v39-" + ZIP_SHA.substring(0, 16));
        try (FileChannel channel = FileChannel.open(parent.resolve("install.lock"), StandardOpenOption.CREATE, StandardOpenOption.WRITE);
             FileLock ignored = channel.lock()) {
            Path existing = target.resolve(LIBRARY);
            if (Files.isRegularFile(existing) && LIBRARY_SHA.equals(hash(existing))) return target;
            Path stage = Files.createTempDirectory(parent, "extract-");
            try {
                log.write("Driver", "Preparando o Turnip incluído no aplicativo");
                Path archive = stage.resolve("driver.zip");
                try (InputStream input = context.getAssets().open(ASSET); OutputStream output = Files.newOutputStream(archive)) {
                    copy(input, output);
                }
                if (!ZIP_SHA.equals(hash(archive))) throw new IOException("O pacote Turnip falhou na verificação de integridade");
                try (ZipFile zip = new ZipFile(archive.toFile())) {
                    java.util.zip.ZipEntry metadata = zip.getEntry("meta.json"), binary = zip.getEntry(LIBRARY);
                    if (metadata == null || binary == null || metadata.getSize() < 0 || metadata.getSize() > 16384)
                        throw new IOException("Pacote Turnip inválido");
                    JSONObject meta;
                    try (InputStream input = zip.getInputStream(metadata)) {
                        ByteArrayOutputStream text = new ByteArrayOutputStream();
                        copy(input, text);
                        meta = new JSONObject(new String(text.toByteArray(), java.nio.charset.StandardCharsets.UTF_8));
                    }
                    if (!TURNIP_LABEL.equals(meta.getString("name")) || !LIBRARY.equals(meta.getString("libraryName")))
                        throw new IOException("O pacote não é o Turnip Gen8 V39-patched esperado");
                    try (InputStream input = zip.getInputStream(binary); OutputStream output = Files.newOutputStream(stage.resolve(LIBRARY))) {
                        copy(input, output);
                    }
                    if (!LIBRARY_SHA.equals(hash(stage.resolve(LIBRARY)))) throw new IOException("Biblioteca Turnip inválida");
                    Files.write(stage.resolve("meta.json"), meta.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                }
                Files.delete(archive);
                if (Files.exists(target)) remove(target);
                Files.move(stage, target, StandardCopyOption.ATOMIC_MOVE);
                return target;
            } catch (JSONException e) {
                throw new IOException("Metadados inválidos no driver Turnip", e);
            } finally { if (Files.exists(stage)) remove(stage); }
        }
    }

    private static void copy(InputStream input, OutputStream output) throws IOException {
        byte[] bytes = new byte[64 * 1024];
        int count;
        while ((count = input.read(bytes)) != -1) {
            if (Thread.currentThread().isInterrupted()) throw new java.io.InterruptedIOException("Preparação do driver cancelada");
            output.write(bytes, 0, count);
        }
    }

    private static String hash(Path file) throws IOException {
        try {
            MessageDigest hash = MessageDigest.getInstance("SHA-256");
            try (InputStream input = Files.newInputStream(file)) {
                byte[] data = new byte[64 * 1024];
                int count;
                while ((count = input.read(data)) != -1) hash.update(data, 0, count);
            }
            StringBuilder hex = new StringBuilder(64);
            for (byte value : hash.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
            return hex.toString();
        } catch (NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    private static void remove(Path root) throws IOException {
        try (Stream<Path> entries = Files.walk(root)) {
            for (Path path : (Iterable<Path>) entries.sorted(Comparator.reverseOrder())::iterator) Files.delete(path);
        }
    }
}
