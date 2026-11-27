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
import android.text.format.Formatter;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.widget.*;
import java.io.IOException;
import java.util.Locale;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;

public final class PortActivity extends Activity {

    private static native String probeGraphics();
    static native String inspect(byte[] sfo);
    private static boolean nativeReady;
    static boolean nativeAvailable() { return nativeReady; }
    private long hostPageSize;

    private static final int SFO = 1, BASE = 2, UPDATE = 3, EXPORT_LOG = 4, INSTALL_PKG = 5;
    private static final int BG = 0xff101116, PANEL = 0xff202128, RED = 0xffff394e,
            WHITE = 0xfffafafa, MUTED = 0xffc0c1ca;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private GameLibrary library;
    private String localStatus = "Selecione a pasta do jogo ou instale um PKG. A atualização separada é opcional.";
    private java.nio.file.Path prepared;
    private boolean preparing;
    private boolean gameLaunched;
    private Button play, rebuild;
    private Button installButton, cancelInstallButton;
    private ProgressBar installProgress;
    private TextView installText;
    private final ArrayList<Button> sourceActions = new ArrayList<>();
    private final AtomicBoolean cancelInstall = new AtomicBoolean();
    private boolean packageBusy;
    private String packageStatus = "Instale o jogo base ou uma atualização a partir de um arquivo PKG local.";
    private int packageProgress;
    private long graphicsQuery;
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
        try {
            System.loadLibrary("driveclub_android");
            nativeReady = true;
        } catch (LinkageError e) {
            log.write("JNI", e.toString());
            localStatus = "Falha ao carregar a biblioteca nativa. Consulte os logs.";
        }
        library = new GameLibrary(this);
        hostPageSize = android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE);
        log.write("Memória", "Página do sistema: " + hostPageSize + " bytes");
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
        queryGraphics();
        prepareLocal(false);
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

    private Button action(LinearLayout box, String label, Runnable callback) {
        Button button = new Button(this, null, android.R.attr.borderlessButtonStyle);
        button.setAllCaps(false);
        button.setText(label);
        button.setTextColor(RED);
        button.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        button.setPadding(0, 0, 0, 0);
        button.setMinHeight(dp(48));
        button.setOnClickListener(v -> callback.run());
        box.addView(button);
        return button;
    }

    private void body() {
        tabs.removeAllViews();
        tab("Biblioteca", false);
        tab("Sistema", true);
        content.removeAllViews();
        pad = null;
        play = null;
        rebuild = null;
        installButton = cancelInstallButton = null;
        installText = null;
        installProgress = null;
        sourceActions.clear();
        preparation = null;
        if (system) {
            LinearLayout device = card("Dispositivo");
            device.addView(text(Build.MANUFACTURER + " " + Build.MODEL + "  ·  Android " + Build.VERSION.RELEASE, 15, MUTED, false));
            device.addView(text("Página de memória: " + hostPageSize + " bytes", 14, MUTED, false));
            if (hostPageSize != 4096)
                device.addView(text("O renderizador desta versão exige páginas de 4 KB.", 14, RED, false));
            LinearLayout graphics = card("Processador gráfico");
            graphics.addView(text("Selecionado: " + DriverManager.label(DriverManager.selected(this)), 15, WHITE, true));
            graphics.addView(text(gpu, 16, WHITE, false));
            action(graphics, "Escolher driver", this::chooseDriver);
            LinearLayout logs = card("Diagnóstico");
            action(logs, "Ver logs", this::showLog);
            action(logs, "Exportar logs", () -> startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT)
                    .addCategory(Intent.CATEGORY_OPENABLE).setType("text/plain")
                    .putExtra(Intent.EXTRA_TITLE, "driveclub-diagnostico.txt"), EXPORT_LOG));
            LinearLayout controls = card("Controles do jogo");
            controls.addView(text("Direção analógica, pedais e botões na tela. Controle físico também é aceito.", 15, MUTED, false));
            action(controls, "Testar controle na tela", () -> startActivity(new Intent(this, ControlsPreviewActivity.class)));
            pad = text(controller, 16, MUTED, false);
            controls.addView(pad);
            action(card("Metadados do jogo"), "Abrir param.sfo", () -> startActivityForResult(
                    new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), SFO));
        } else {
            folderCard("Pasta do jogo", GameLibrary.BASE, BASE);
            folderCard("Atualização separada (opcional)", GameLibrary.UPDATE, UPDATE);
            LinearLayout packages = card("Instalar por PKG");
            installButton = action(packages, "Instalar / atualizar por PKG", () -> {
                if (packageBusy || preparing) return;
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                        .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), INSTALL_PKG);
            });
            installText = text(packageStatus, 14, MUTED, false);
            packages.addView(installText);
            if (packageBusy) {
                installProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
                installProgress.setMax(1000);
                installProgress.setProgress(packageProgress);
                packages.addView(installProgress, -1, dp(16));
                cancelInstallButton = action(packages, cancelInstall.get() ? "Cancelando…" : "Cancelar instalação", () -> {
                    cancelInstall.set(true);
                    packageStatus = "Cancelando a instalação…";
                    updatePackageProgress();
                });
            }
            LinearLayout launch = card("Jogar");
            play = action(launch, "Iniciar Driveclub", this::startGame);
            rebuild = action(launch, "Preparar novamente", () -> prepareLocal(true));
            preparation = text(localStatus, 14, MUTED, false);
            content.addView(preparation);
            updateActions();
        }
    }

    private void prepareLocal(boolean refresh) {
        Uri base = library.folder(GameLibrary.BASE);
        Uri update = library.effectiveUpdate();
        if (!nativeReady || base == null || packageBusy) return;
        prepared = null;
        preparing = true;
        setPreparation("Verificando os arquivos locais…");
        worker.execute(() -> {
            try {
                java.nio.file.Path ready = LocalGame.prepare(getApplicationContext(), library, base, update, refresh, log,
                        (stage, message) -> {
                            String[] steps = {"Verificando as pastas…", "Lendo os módulos…", "Convertendo o executável…",
                                    "Vinculando os módulos…", "Preparando o conteúdo…", "Aplicando os patches…", "Preparação concluída."};
                            String status = stage >= 0 && stage < steps.length ? steps[stage] : message;
                            log.write("Preparação local", status);
                            runOnUiThread(() -> {
                                if (!isDestroyed() && library.matches(base, update)) setPreparation(status);
                            });
                        });
                runOnUiThread(() -> {
                    if (!isDestroyed() && library.matches(base, update)) {
                        prepared = ready;
                        preparing = false;
                        setPreparation(ready == null ? "Selecione novamente as pastas do jogo." :
                                BuildConfig.GAME_RUNTIME ? (hostPageSize == 4096 ?
                                        "Pronto para iniciar. Os arquivos foram preparados neste aparelho." :
                                        "Preparação concluída. O renderizador ainda não suporta o tamanho de página deste aparelho.") :
                                "Preparação concluída. Esta compilação de teste não contém o runtime ARM64.");
                    }
                });
            } catch (IOException | RuntimeException e) {
                String message = e.getMessage();
                log.write("Preparação local", message == null ? e.getClass().getSimpleName() : message);
                runOnUiThread(() -> {
                    if (!isDestroyed() && library.matches(base, update)) {
                        preparing = false;
                        setPreparation("Não foi possível preparar: " + (message == null ? "consulte os logs." : message));
                    }
                });
            }
        });
    }

    private void setPreparation(String text) {
        localStatus = text;
        if (preparation != null) preparation.setText(text);
        updateActions();
    }

    private void updateActions() {
        enabled(play, nativeReady && BuildConfig.GAME_RUNTIME && hostPageSize == 4096 && prepared != null && !preparing && !packageBusy);
        enabled(rebuild, nativeReady && !preparing && !packageBusy && library.folder(GameLibrary.BASE) != null);
        enabled(installButton, !packageBusy && !preparing);
        for (Button button : sourceActions) enabled(button, !packageBusy && !preparing);
        enabled(cancelInstallButton, packageBusy && !cancelInstall.get());
    }

    private void enabled(Button button, boolean value) {
        if (button != null) { button.setEnabled(value); button.setAlpha(value ? 1f : .4f); }
    }

    private void startGame() {
        if (prepared == null || preparing || packageBusy || !BuildConfig.GAME_RUNTIME) return;
        log.write("Execução", "Iniciando Driveclub com Bionic e FEX no processo do jogo");
        gameLaunched = true;
        startActivity(new Intent(this, GameActivity.class).putExtra("prepared", prepared.toString())
                .putExtra("base", library.folder(GameLibrary.BASE).toString())
                .putExtra("update", library.effectiveUpdate().toString()));
    }

    private void folderCard(String title, String key, int request) {
        LinearLayout box = card(title);
        boolean selected = library.folder(key) != null;
        TextView result = text(selected ? "Verificando pasta…" :
                (key.equals(GameLibrary.BASE) ? "Pode conter o jogo já atualizado para 1.28." :
                "Deixe vazio se a atualização já está na pasta do jogo."), 15, MUTED, false);
        box.addView(result);
        sourceActions.add(action(box, selected ? "Trocar pasta  ›" : "Escolher pasta  ›", () -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                        | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION), request)));
        if (GameLibrary.UPDATE.equals(key) && selected)
            sourceActions.add(action(box, "Usar atualização da pasta do jogo", () -> {
                try { library.clearUpdate(); prepared = null; body(); prepareLocal(true); }
                catch (IOException e) { notice("Pasta", e.getMessage()); }
            }));
        if (selected) worker.execute(() -> {
            String message;
            try { message = library.summary(key); }
            catch (IOException | RuntimeException e) { message = "Pasta inacessível. Selecione novamente. (" + e.getClass().getSimpleName() + ")"; }
            log.write(key, message);
            String status = message;
            runOnUiThread(() -> result.setText(status));
        });
    }

    private void queryGraphics() {
        if (!nativeReady) return;
        final long query = ++graphicsQuery;
        final String choice = DriverManager.selected(this);
        gpu = "Consultando " + DriverManager.label(choice) + "…";
        worker.execute(() -> {
            String result;
            try {
                DriverManager.configure(this, log, choice);
                log.write("Vulkan", "Consultando dispositivos com " + DriverManager.label(choice));
                result = probeGraphics();
            } catch (IOException | RuntimeException | LinkageError e) {
                result = "Não foi possível iniciar o driver selecionado: " + e.getMessage();
            }
            log.write("Vulkan", DriverManager.label(choice) + ": " + result);
            String value = result;
            runOnUiThread(() -> {
                if (query != graphicsQuery || isDestroyed()) return;
                gpu = value;
                if (system) body();
            });
        });
    }

    private void chooseDriver() {
        String[] labels = {DriverManager.TURNIP_LABEL + " (incluído)", "Driver do sistema"};
        int selected = DriverManager.SYSTEM.equals(DriverManager.selected(this)) ? 1 : 0;
        new AlertDialog.Builder(this).setTitle("Driver Vulkan")
                .setSingleChoiceItems(labels, selected, (dialog, index) -> {
                    try {
                        DriverManager.select(this, index == 0 ? DriverManager.TURNIP : DriverManager.SYSTEM);
                        dialog.dismiss();
                        queryGraphics();
                        body();
                    } catch (IOException e) { notice("Driver", e.getMessage()); }
                }).setNegativeButton("Fechar", null).show();
    }

    private void inspectPackage(Uri uri) {
        if (packageBusy || preparing) return;
        cancelInstall.set(false);
        packageBusy = true;
        packageProgress = 0;
        packageStatus = "Lendo os metadados do PKG…";
        body();
        worker.execute(() -> {
            try {
                PkgInstaller.Plan plan = PkgInstaller.inspect(this, uri);
                if (cancelInstall.get()) throw new PkgInstaller.CancelledException();
                if (plan.isUpdate && library.folder(GameLibrary.BASE) == null)
                    throw new IOException("Selecione a pasta do jogo ou instale o PKG base antes da atualização.");
                if (plan.isUpdate && !plan.version.matches("0*1\\.28"))
                    throw new IOException("Este runtime usa a atualização 1.28. O PKG selecionado é " + plan.version + ".");
                runOnUiThread(() -> {
                    if (isDestroyed() || isFinishing()) return;
                    packageBusy = false;
                    packageStatus = "PKG identificado: " + plan.titleId + " · " + plan.version;
                    body();
                    String type = plan.isUpdate ? "Atualização" : "Jogo base";
                    String size = plan.extractedBytes > 0 ? Formatter.formatFileSize(this, plan.extractedBytes) : "a verificar";
                    String message = type + " · " + plan.title + "\n" + plan.titleId + " · versão " + plan.version
                            + "\nEspaço para instalação: " + size
                            + "\n\n" + (plan.isUpdate ? "A atualização será usada com o jogo selecionado."
                            : "A nova instalação ficará selecionada no aplicativo.") + " Seus saves serão mantidos.";
                    new AlertDialog.Builder(this).setTitle(plan.isUpdate ? "Instalar atualização" : "Instalar jogo")
                            .setMessage(message).setNegativeButton("Cancelar", null)
                            .setPositiveButton("Instalar", (dialog, which) -> installPackage(uri, plan)).show();
                });
            } catch (IOException | RuntimeException | LinkageError e) {
                finishPackageError(e);
            }
        });
    }

    private void installPackage(Uri uri, PkgInstaller.Plan plan) {
        if (packageBusy || preparing) return;
        cancelInstall.set(false);
        packageBusy = true;
        packageProgress = 0;
        packageStatus = "Instalando " + plan.titleId + " · " + plan.version + "…";
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        body();
        log.write("PKG", "Instalando " + plan.titleId + " " + plan.version + (plan.isUpdate ? " (atualização)" : " (base)"));
        worker.execute(() -> {
            PkgInstaller.Result result = null;
            boolean published = false;
            try {
                final long[] lastUi = {0};
                result = PkgInstaller.install(this, uri, plan, (done, total, path) -> {
                    if (cancelInstall.get() || Thread.currentThread().isInterrupted()) return false;
                    long now = android.os.SystemClock.uptimeMillis();
                    if (done == total || now - lastUi[0] >= 250) {
                        lastUi[0] = now;
                        int progress = total > 0 ? (int) Math.min(1000, (double) done * 1000 / total) : 0;
                        String status = Formatter.formatFileSize(this, Math.max(0, done))
                                + (total > 0 ? " / " + Formatter.formatFileSize(this, total) : "")
                                + (path == null || path.isEmpty() ? "" : "\n" + path);
                        runOnUiThread(() -> {
                            if (isDestroyed()) return;
                            packageProgress = progress;
                            if (!cancelInstall.get()) packageStatus = status;
                            updatePackageProgress();
                        });
                    }
                    return true;
                });
                if (cancelInstall.get() || Thread.currentThread().isInterrupted()) throw new PkgInstaller.CancelledException();
                library.publish(result, log);
                published = true;
                log.write("PKG", "Instalação concluída e ativada: " + result.titleId + " " + result.version);
                runOnUiThread(() -> {
                    if (isDestroyed()) return;
                    packageBusy = false;
                    packageProgress = 1000;
                    packageStatus = (plan.isUpdate ? "Atualização " : "Jogo ") + plan.version + " instalado neste aparelho.";
                    prepared = null;
                    getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    body();
                    prepareLocal(true);
                });
            } catch (IOException | RuntimeException | LinkageError e) {
                finishPackageError(e);
            } finally {
                if (!published && result != null) library.discard(result, log);
            }
        });
    }

    private void finishPackageError(Throwable error) {
        String message = error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
        log.write("PKG", message);
        runOnUiThread(() -> {
            if (isDestroyed()) return;
            packageBusy = false;
            packageStatus = message;
            getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            body();
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        });
    }

    private void updatePackageProgress() {
        if (installText != null) installText.setText(packageStatus);
        if (installProgress != null) installProgress.setProgress(packageProgress);
        if (cancelInstallButton != null && cancelInstall.get()) cancelInstallButton.setText("Cancelando…");
        updateActions();
    }

    private void notice(String tag, String message) {
        String text = message == null ? "Não foi possível concluir a operação." : message;
        log.write(tag, text);
        Toast.makeText(this, text, Toast.LENGTH_LONG).show();
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
            catch (IOException | SecurityException e) { notice("Pasta", "Falha ao selecionar a pasta: " + e.getMessage()); }
            body();
        } else if (request == INSTALL_PKG) {
            inspectPackage(uri);
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

    @Override protected void onResume() {
        super.onResume();
        if (gameLaunched) {
            gameLaunched = false;
            worker.execute(() -> log.recordExits(this));
        }
    }

    @Override protected void onDestroy() {
        cancelInstall.set(true);
        worker.shutdownNow();
        super.onDestroy();
    }
}
