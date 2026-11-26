package org.driveclub.port;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class PortActivity extends Activity {
    static { System.loadLibrary("driveclub_android"); }
    private static native String probeGraphics();
    private static native String inspect(byte[] sfo);
    private static final int PICK_SFO = 1;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private TextView result;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(16 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        TextView graphics = new TextView(this);
        graphics.setText("Verificando Vulkan…");
        layout.addView(graphics);
        Button pick = new Button(this);
        pick.setText("Verificar param.sfo");
        pick.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK_SFO));
        layout.addView(pick);
        result = new TextView(this);
        result.setText(state == null ? "Selecione sce_sys/param.sfo da atualização para verificar título e versão."
                : state.getString("result"));
        layout.addView(result);
        setContentView(layout);
        worker.execute(() -> {
            String status = probeGraphics();
            runOnUiThread(() -> graphics.setText(status));
        });
    }

    @Override protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (request != PICK_SFO || code != RESULT_OK || data == null || data.getData() == null) return;
        worker.execute(() -> {
            String status;
            try (InputStream input = getContentResolver().openInputStream(data.getData())) {
                if (input == null) throw new IOException();
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] block = new byte[4096];
                int length;
                while ((length = input.read(block)) != -1) {
                    if (bytes.size() + length > 65536) throw new IOException();
                    bytes.write(block, 0, length);
                }
                status = inspect(bytes.toByteArray());
            } catch (IOException | SecurityException error) {
                status = "Falha ao ler o SFO. Selecione um arquivo acessível de até 64 KiB.";
            }
            String message = status;
            runOnUiThread(() -> result.setText(message));
        });
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putString("result", result.getText().toString());
    }

    @Override protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}
