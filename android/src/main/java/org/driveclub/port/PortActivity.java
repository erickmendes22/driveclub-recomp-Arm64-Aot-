package org.driveclub.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.widget.*;
import java.io.IOException;
import java.util.Locale;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;

public final class PortActivity extends Activity {

    private static native String probeGraphics();
    static native String inspect(byte[] sfo);

    private static final int SFO = 1, BASE = 2, UPDATE = 3, EXPORT_LOG = 4;
    private static final int BG = 0xff101116, PANEL = 0xff202128, RED = 0xffff394e,
            WHITE = 0xfffafafa, MUTED = 0xffc0c1ca;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private GameLibrary library;
    private String localStatus = "Execução do jogo em desenvolvimento.";
    private TextView preparation;
    private PortLog log;
    private LinearLayout page, tabs, content;
    private TextView pad;
    private boolean system;
    private String gpu = "Consultando o driver…";
    private String controller = "Mova o analógico ou pressione um gatilho.";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        log = new PortLog(this);
        log.session(this);
        log.write("JNI", "Carregando biblioteca nativa");
        System.loadLibrary("driveclub_android");
        library = new GameLibrary(this);
        system = state != null && state.getBoolean("system");
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.setFillViewport(true);
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets bars = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                    | android.view.WindowInsets.Type.displayCutout());
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return insets;
        });
        getWindow().setDecorFitsSystemWindows(false);
        page = column();
        scroll.addView(page);
        setContentView(scroll);
        render();
        prepareLocal(false);
        worker.execute(() -> {
            log.write("Vulkan", "Consultando dispositivos");
            String value = probeGraphics();
            log.write("Vulkan", value);
            runOnUiThread(() -> { gpu = value; if (system && !isDestroyed()) body(); });
        });
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    private LinearLayout column() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        return box;
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setTypeface(Typeface.create(bold ? "sans-serif-condensed" : "sans-serif",
                bold ? Typeface.BOLD : Typeface.NORMAL));
        return view;
    }

    private void inset(View view, int x, int y) { view.setPadding(dp(x), dp(y), dp(x), dp(y)); }

    private void render() {
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.driveclub_wordmark);
        logo.setContentDescription("Driveclub");
        logo.setScaleType(ImageView.ScaleType.FIT_START);
        inset(logo, 20, 14);
        page.addView(logo, -1, dp(51));

        FrameLayout hero = new FrameLayout(this);
        ImageView photo = new ImageView(this);
        photo.setImageResource(R.drawable.garage_cover);
        photo.setScaleType(ImageView.ScaleType.CENTER_CROP);
        hero.addView(photo, -1, -1);
        View shade = new View(this);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[]{0x00000000, 0x30000000, 0xe8101116}));
        hero.addView(shade, -1, -1);
        TextView title = text("Garagem", 44, WHITE, true);
        inset(title, 20, 8);
        hero.addView(title, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        page.addView(hero, -1, dp(getResources().getConfiguration().screenHeightDp < 500 ? 118 : 178));
        tabs = new LinearLayout(this);
        page.addView(tabs);
        content = column();
        inset(content, 18, 14);
        page.addView(content);
        body();
    }

    private void tab(String label, boolean target) {
        LinearLayout container = column();
        TextView button = text(label, 17, system == target ? WHITE : MUTED, true);
        button.setGravity(Gravity.CENTER);
        container.addView(button, -1, dp(48));
        View line = new View(this);
        line.setBackgroundColor(system == target ? RED : PANEL);
        container.addView(line, -1, dp(3));
        container.setSelected(system == target);
        container.setFocusable(true);
        container.setContentDescription(label);
        container.setOnClickListener(v -> { system = target; body(); });
        tabs.addView(container, new LinearLayout.LayoutParams(0, -2, 1));
    }

    private LinearLayout card(String title) {
        LinearLayout box = column();
        inset(box, 16, 12);
        box.setBackgroundColor(PANEL);
        LinearLayout.LayoutParams spacing = new LinearLayout.LayoutParams(-1, -2);
        spacing.bottomMargin = dp(10);
        content.addView(box, spacing);
        box.addView(text(title, 21, WHITE, true));
        return box;
    }

    private void action(LinearLayout box, String label, Runnable callback) {
        Button button = new Button(this, null, android.R.attr.borderlessButtonStyle);
        button.setAllCaps(false);
        button.setText(label);
        button.setTextColor(RED);
        button.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        button.setPadding(0, 0, 0, 0);
        button.setMinHeight(dp(48));
        button.setOnClickListener(v -> callback.run());
        box.addView(button);
    }

    private void body() {
        tabs.removeAllViews();
        tab("Biblioteca", false);
        tab("Sistema", true);
        content.removeAllViews();
        pad = null;
        if (system) {
            LinearLayout device = card("Dispositivo");
            device.addView(text(Build.MANUFACTURER + " " + Build.MODEL + "  ·  Android " + Build.VERSION.RELEASE, 15, MUTED, false));
            LinearLayout graphics = card("Processador gráfico");
            graphics.addView(text(gpu, 16, WHITE, false));
            LinearLayout logs = card("Diagnóstico");
            action(logs, "Ver logs", this::showLog);
            action(logs, "Exportar logs", () -> startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT)
                    .addCategory(Intent.CATEGORY_OPENABLE).setType("text/plain")
                    .putExtra(Intent.EXTRA_TITLE, "driveclub-diagnostico.txt"), EXPORT_LOG));
            LinearLayout controls = card("Teste de controle");
            pad = text(controller, 16, MUTED, false);
            controls.addView(pad);
            action(card("Metadados do jogo"), "Abrir param.sfo", () -> startActivityForResult(
                    new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), SFO));
        } else {
            folderCard("Jogo base", GameLibrary.BASE, BASE);
            folderCard("Atualização 1.28", GameLibrary.UPDATE, UPDATE);
            preparation = text(localStatus, 14, MUTED, false);
            content.addView(preparation);
        }
    }

    private void prepareLocal(boolean refresh) {
        Uri base = library.folder(GameLibrary.BASE);
        Uri update = library.folder(GameLibrary.UPDATE);
        if (base == null || update == null) return;
        setPreparation("Verificando os arquivos locais…");
        worker.execute(() -> {
            try {
                LocalGame.prepare(getApplicationContext(), library, base, update, refresh, log);
                runOnUiThread(() -> {
                    if (base.equals(library.folder(GameLibrary.BASE)) && update.equals(library.folder(GameLibrary.UPDATE)))
                        setPreparation("Arquivos preparados neste aparelho. Execução em desenvolvimento.");
                });
            } catch (IOException | RuntimeException e) {
                String message = e.getMessage();
                log.write("Preparação local", message == null ? e.getClass().getSimpleName() : message);
                runOnUiThread(() -> {
                    if (base.equals(library.folder(GameLibrary.BASE)) && update.equals(library.folder(GameLibrary.UPDATE)))
                        setPreparation("Falha na preparação local. Consulte os logs.");
                });
            }
        });
    }

    private void setPreparation(String text) {
        localStatus = text;
        if (preparation != null) preparation.setText(text);
    }

    private void folderCard(String title, String key, int request) {
        LinearLayout box = card(title);
        boolean selected = library.folder(key) != null;
        TextView result = text(selected ? "Verificando pasta…" :
                (key.equals(GameLibrary.BASE) ? "Selecione a pasta que contém game000.dat." :
                "Selecione a pasta que contém sce_sys."), 15, MUTED, false);
        box.addView(result);
        action(box, selected ? "Trocar pasta  ›" : "Escolher pasta  ›", () -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                        | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION), request));
        if (selected) worker.execute(() -> {
            String message;
            try { message = library.summary(key); }
            catch (IOException | RuntimeException e) { message = "Pasta inacessível. Selecione novamente. (" + e.getClass().getSimpleName() + ")"; }
            log.write(key, message);
            String status = message;
            runOnUiThread(() -> result.setText(status));
        });
    }

    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (pad == null || !event.isFromSource(InputDevice.SOURCE_JOYSTICK)
                || event.getAction() != MotionEvent.ACTION_MOVE) return super.onGenericMotionEvent(event);
        float steer = event.getAxisValue(MotionEvent.AXIS_X);
        float gas = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
        float brake = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
        controller = String.format(Locale.getDefault(), "Direção %+.0f%%   ·   R2 %.0f%%   ·   L2 %.0f%%",
                steer * 100, gas * 100, brake * 100);
        pad.setText(controller);
        return true;
    }

    @Override protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (code != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (request == EXPORT_LOG) {
            worker.execute(() -> exportLog(uri));
        } else if (request == BASE || request == UPDATE) {
            try {
                library.select(request == BASE ? GameLibrary.BASE : GameLibrary.UPDATE, uri);
                prepareLocal(true);
            }
            catch (SecurityException e) { log.write("Pasta", "Permissão recusada"); Toast.makeText(this, "Falha ao obter acesso à pasta.", Toast.LENGTH_LONG).show(); }
            body();
        } else if (request == SFO) worker.execute(() -> {
            String status;
            try { status = library.inspectFile(uri); }
            catch (IOException | RuntimeException e) { status = "Falha ao ler o SFO (limite de 64 KiB)."; }
            log.write("SFO", status);
            String result = status;
            runOnUiThread(() -> {
                if (!isFinishing() && !isDestroyed()) new AlertDialog.Builder(this).setTitle("param.sfo")
                        .setMessage(result).setPositiveButton("Fechar", null).show();
            });
        });
    }

    private void showLog() {
        worker.execute(() -> {
            String message;
            try {
                String history = new String(log.snapshot(), java.nio.charset.StandardCharsets.UTF_8);
                int cut = history.length() - 12000;
                message = cut > 0 ? history.substring(history.indexOf('\n', cut) + 1) : history;
            } catch (IOException e) { message = "Falha ao ler os logs."; }
            String visible = message;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                new AlertDialog.Builder(this).setTitle("Logs recentes").setMessage(visible)
                        .setPositiveButton("Fechar", null).show();
            });
        });
    }

    private void exportLog(Uri uri) {
        String result;
        try {
            byte[] data = log.snapshot();
            try (java.io.OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
                if (out == null) throw new IOException();
                out.write(data);
            }
            result = "Logs exportados.";
        } catch (IOException | RuntimeException e) {
            result = "Falha ao exportar os logs (" + e.getClass().getSimpleName() + ").";
        }
        log.write("Exportação", result);
        String notice = result;
        runOnUiThread(() -> Toast.makeText(this, notice, Toast.LENGTH_LONG).show());
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putBoolean("system", system);
    }

    @Override protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}
