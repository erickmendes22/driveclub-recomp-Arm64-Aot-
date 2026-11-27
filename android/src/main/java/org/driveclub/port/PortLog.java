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
    private final File current, previous;
    private static final int LIMIT = 128 * 1024;

    PortLog(Context context) {
        current = new File(context.getFilesDir(), "driveclub.log");
        previous = new File(context.getFilesDir(), "driveclub.previous.log");
    }

    void session(Context context) {
        write("Sessão", "Driveclub " + BuildConfig.VERSION_NAME + " · Android " + Build.VERSION.RELEASE
                + " · " + Build.MANUFACTURER + " " + Build.MODEL + " · " + Build.SUPPORTED_ABIS[0]);
        try {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(null, 0, 1);
            if (!exits.isEmpty()) {
                ApplicationExitInfo exit = exits.get(0);
                String reason;
                switch (exit.getReason()) {
                    case ApplicationExitInfo.REASON_CRASH: reason = "falha Java"; break;
                    case ApplicationExitInfo.REASON_CRASH_NATIVE: reason = "falha nativa"; break;
                    case ApplicationExitInfo.REASON_ANR: reason = "aplicativo sem resposta"; break;
                    case ApplicationExitInfo.REASON_LOW_MEMORY: reason = "memória insuficiente"; break;
                    default: reason = "motivo Android " + exit.getReason();
                }
                write("Saída anterior", Instant.ofEpochMilli(exit.getTimestamp()) + " · " + reason
                        + " · status " + exit.getStatus());
            }
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
            Files.copy(current.toPath(), result);
            return result.toByteArray();
        }
    }
}
