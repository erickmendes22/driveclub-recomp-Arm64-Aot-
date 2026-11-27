package org.driveclub.port;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import android.util.Log;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;
import java.util.List;

final class PortLog {
    private final File current, previous, runtime, previousRuntime;
    private static final int LIMIT = 128 * 1024;

    PortLog(Context context) {
        current = new File(context.getFilesDir(), "driveclub.log");
        previous = new File(context.getFilesDir(), "driveclub.previous.log");
        runtime = new File(context.getFilesDir(), "driveclub-runtime.log");
        previousRuntime = new File(context.getFilesDir(), "driveclub-runtime.previous.log");
    }

    void session(Context context) {
        write("Sessão", "Driveclub " + BuildConfig.VERSION_NAME + " · Android " + Build.VERSION.RELEASE
                + " · " + Build.MANUFACTURER + " " + Build.MODEL + " · " + Build.SUPPORTED_ABIS[0]);
        recordExits(context);
    }

    void recordExits(Context context) {
        try {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(null, 0, 8);
            android.content.SharedPreferences state = context.getSharedPreferences("diagnostic-exits", Context.MODE_PRIVATE);
            long last = state.getLong("recorded", 0), newest = last;
            for (int i = exits.size() - 1; i >= 0; --i) {
                ApplicationExitInfo exit = exits.get(i);
                if (exit.getTimestamp() <= last) continue;
                String reason;
                switch (exit.getReason()) {
                    case ApplicationExitInfo.REASON_CRASH: reason = "falha Java"; break;
                    case ApplicationExitInfo.REASON_CRASH_NATIVE: reason = "falha nativa"; break;
                    case ApplicationExitInfo.REASON_ANR: reason = "aplicativo sem resposta"; break;
                    case ApplicationExitInfo.REASON_LOW_MEMORY: reason = "memória insuficiente"; break;
                    default: reason = "motivo Android " + exit.getReason();
                }
                write("Saída anterior", Instant.ofEpochMilli(exit.getTimestamp()) + " · " + reason
                        + " · " + exit.getProcessName() + " · status " + exit.getStatus()
                        + " · RSS " + exit.getRss() + " KiB · " + exit.getDescription());
                newest = Math.max(newest, exit.getTimestamp());
            }
            if (newest > last) state.edit().putLong("recorded", newest).apply();
        } catch (RuntimeException e) {
            write("Histórico Android", e.getClass().getSimpleName());
        }
    }

    void write(String source, String message) {
        String line = Instant.now() + " [" + source + "] " + message.replace('\n', ' ').replace('\r', ' ') + "\n";
        synchronized (PortLog.class) {
            try {
                if (current.length() >= LIMIT) {
                    Files.move(current.toPath(), previous.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                }
                try (FileOutputStream out = new FileOutputStream(current, true)) {
                    out.write(line.getBytes(StandardCharsets.UTF_8));
                }
            } catch (IOException e) {
                Log.e("Driveclub", "Falha ao registrar diagnóstico", e);
            }
        }
    }

    byte[] snapshot() throws IOException {
        synchronized (PortLog.class) {
            ByteArrayOutputStream result = new ByteArrayOutputStream();
            if (previous.isFile()) Files.copy(previous.toPath(), result);
            if (current.isFile()) Files.copy(current.toPath(), result);
            appendRuntime(result, previousRuntime, "Execução nativa anterior");
            appendRuntime(result, runtime, "Execução nativa atual");
            return result.toByteArray();
        }
    }

    private static void appendRuntime(ByteArrayOutputStream output, File file, String title) throws IOException {
        if (!file.isFile()) return;
        output.write(("\n=== " + title + " ===\n").getBytes(StandardCharsets.UTF_8));
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            // A broken guest can log indefinitely. Keep export usable after such a
            // failure while preserving the final native crash and guest backtrace.
            long start = Math.max(0, input.length() - 8L * 1024 * 1024);
            if (start > 0) {
                output.write("[Exibindo os últimos 8 MiB desta execução]\n".getBytes(StandardCharsets.UTF_8));
                input.seek(start);
                input.readLine();
            }
            byte[] buffer = new byte[16384];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
        }
    }
}
