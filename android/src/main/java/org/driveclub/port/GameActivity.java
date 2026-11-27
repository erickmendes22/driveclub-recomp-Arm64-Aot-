package org.driveclub.port;

import android.app.Activity;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/** Owns the game's native process. Guest exit/crash never takes down the garage. */
public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    private static native int runNative(String preparedDir, String userDir, String logPath,
                                        RuntimeFiles files, Surface surface);
    private static native void inputNative(int buttons, float lx, float ly, float rx, float ry, float l2, float r2);
    private boolean started, loaded, ending;
    private PortLog log;
    private RuntimeFiles files;
    private int keyButtons, touchButtons;
    private float lx, ly, rx, ry, leftTrigger, rightTrigger;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        log = new PortLog(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.hide(WindowInsets.Type.systemBars());
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout top = row();
        Button close = button("Sair", "Voltar à garagem");
        close.setOnClickListener(v -> endRuntime("Encerrado pelo usuário"));
        top.addView(close);
        TextView hint = new TextView(this);
        hint.setText("Driveclub"); hint.setTextColor(0xccffffff); hint.setPadding(dp(12), 0, 0, 0);
        top.addView(hint);
        FrameLayout.LayoutParams topParams = new FrameLayout.LayoutParams(-2, dp(48), Gravity.TOP | Gravity.START);
        topParams.topMargin = dp(8); topParams.leftMargin = dp(12);
        root.addView(top, topParams);

        LinearLayout left = column();
        LinearLayout arrows = row();
        touch(arrows, "←", "Direcional esquerdo", 0x80);
        touch(arrows, "↑", "Direcional para cima", 0x10);
        touch(arrows, "↓", "Direcional para baixo", 0x40);
        touch(arrows, "→", "Direcional direito", 0x20);
        left.addView(arrows);
        LinearLayout shoulders = row();
        touch(shoulders, "L1", "L1", 0x400);
        touch(shoulders, "L2", "Frear", 0x100);
        touch(shoulders, "≡", "Options", 0x8);
        left.addView(shoulders);
        placeControls(root, left, Gravity.START);

        LinearLayout right = column();
        LinearLayout faces = row();
        touch(faces, "□", "Quadrado", 0x8000);
        touch(faces, "△", "Triângulo", 0x1000);
        touch(faces, "○", "Círculo", 0x2000);
        touch(faces, "×", "Cruz", 0x4000);
        right.addView(faces);
        LinearLayout triggers = row();
        touch(triggers, "R1", "R1", 0x800);
        touch(triggers, "R2", "Acelerar", 0x200);
        right.addView(triggers);
        placeControls(root, right, Gravity.END);

        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets cutout = insets.getInsets(WindowInsets.Type.displayCutout());
            v.setPadding(cutout.left, cutout.top, cutout.right, cutout.bottom);
            return insets;
        });
        setContentView(root);
        try {
            if (!BuildConfig.GAME_RUNTIME) throw new UnsatisfiedLinkError("Runtime ARM64 ausente nesta compilação");
            System.loadLibrary("driveclub_game");
            loaded = true;
            sendInput();
        } catch (LinkageError e) {
            log.write("Runtime", e.toString());
            Toast.makeText(this, "Falha ao carregar o runtime. Consulte os logs.", Toast.LENGTH_LONG).show();
            endRuntime("Falha ao carregar o runtime");
        }
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private LinearLayout row() {
        LinearLayout row = new LinearLayout(this); row.setGravity(Gravity.CENTER_VERTICAL); return row;
    }
    private LinearLayout column() {
        LinearLayout column = row(); column.setOrientation(LinearLayout.VERTICAL); return column;
    }
    private Button button(String label, String description) {
        Button button = new Button(this);
        button.setText(label); button.setTextSize(18); button.setAllCaps(false);
        button.setContentDescription(description); button.setTextColor(Color.WHITE);
        button.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0x77202128));
        button.setMinWidth(0); button.setMinimumWidth(0); button.setPadding(0, 0, 0, 0);
        button.setLayoutParams(new LinearLayout.LayoutParams(dp(52), dp(48)));
        return button;
    }
    private void placeControls(FrameLayout root, View controls, int side) {
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | side);
        params.setMargins(dp(12), 0, dp(12), dp(12)); root.addView(controls, params);
    }
    private void touch(LinearLayout row, String label, String description, int mask) {
        Button button = button(label, description);
        button.setOnTouchListener((view, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                touchButtons |= mask; view.setPressed(true); sendInput(); return true;
            }
            if (event.getActionMasked() == MotionEvent.ACTION_UP || event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
                touchButtons &= ~mask; view.setPressed(false); sendInput();
                if (event.getActionMasked() == MotionEvent.ACTION_UP) view.performClick();
                return true;
            }
            return true;
        });
        row.addView(button);
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        if (!loaded || started || ending) return;
        started = true;
        new Thread(() -> {
            try {
                Path prepared = new File(getIntent().getStringExtra("prepared")).toPath().toRealPath();
                Path root = getNoBackupFilesDir().toPath().resolve("driveclub").toRealPath();
                if (!prepared.startsWith(root) || !Files.isRegularFile(prepared.resolve("work/prep/boot-linked.bin")))
                    throw new java.io.IOException("Preparação local inválida");
                Path user = Files.createDirectories(root.resolve("user"));
                Path current = getFilesDir().toPath().resolve("driveclub-runtime.log");
                if (Files.exists(current)) Files.move(current, getFilesDir().toPath().resolve("driveclub-runtime.previous.log"),
                        StandardCopyOption.REPLACE_EXISTING);
                files = new RuntimeFiles(this, Uri.parse(getIntent().getStringExtra("base")),
                        Uri.parse(getIntent().getStringExtra("update")));
                log.write("Runtime", "Entrando no carregador Bionic/FEX; conteúdo aberto diretamente das pastas escolhidas");
                int result = runNative(prepared.toString(), user.toString(), current.toString(), files, holder.getSurface());
                runOnUiThread(() -> {
                    Toast.makeText(this, "O jogo encerrou (" + result + "). Consulte os logs.", Toast.LENGTH_LONG).show();
                    endRuntime("Carregador retornou " + result);
                });
            } catch (Exception | LinkageError e) {
                log.write("Runtime", e.toString());
                runOnUiThread(() -> {
                    Toast.makeText(this, "Falha ao iniciar. Consulte os logs.", Toast.LENGTH_LONG).show();
                    endRuntime("Falha na inicialização");
                });
            }
        }, "driveclub-runtime").start();
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        if (started) endRuntime("Superfície do jogo encerrada");
    }

    private void sendInput() {
        if (!loaded || ending) return;
        int buttons = keyButtons | touchButtons;
        float steering = (touchButtons & 0x80) != 0 ? -1 : (touchButtons & 0x20) != 0 ? 1 : lx;
        inputNative(buttons, steering, ly, rx, ry,
                Math.max(leftTrigger, (buttons & 0x100) != 0 ? 1 : 0),
                Math.max(rightTrigger, (buttons & 0x200) != 0 ? 1 : 0));
    }
    private void clearInput() {
        keyButtons = touchButtons = 0;
        lx = ly = rx = ry = leftTrigger = rightTrigger = 0;
        sendInput();
    }
    private static int keyMask(int key) {
        switch (key) {
            case KeyEvent.KEYCODE_BUTTON_A: return 0x4000;
            case KeyEvent.KEYCODE_BUTTON_B: return 0x2000;
            case KeyEvent.KEYCODE_BUTTON_X: return 0x8000;
            case KeyEvent.KEYCODE_BUTTON_Y: return 0x1000;
            case KeyEvent.KEYCODE_BUTTON_L1: return 0x400;
            case KeyEvent.KEYCODE_BUTTON_R1: return 0x800;
            case KeyEvent.KEYCODE_BUTTON_L2: return 0x100;
            case KeyEvent.KEYCODE_BUTTON_R2: return 0x200;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 0x2;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 0x4;
            case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_MENU: return 0x8;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 0x100000;
            case KeyEvent.KEYCODE_DPAD_UP: return 0x10;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x20;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0x40;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0x80;
            default: return 0;
        }
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int mask = keyMask(event.getKeyCode());
        if (mask != 0 && (event.getAction() == KeyEvent.ACTION_DOWN || event.getAction() == KeyEvent.ACTION_UP)) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) keyButtons |= mask;
            else keyButtons &= ~mask;
            sendInput(); return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (event.isFromSource(InputDevice.SOURCE_JOYSTICK) && event.getAction() == MotionEvent.ACTION_MOVE) {
            lx = event.getAxisValue(MotionEvent.AXIS_X); ly = event.getAxisValue(MotionEvent.AXIS_Y);
            rx = event.getAxisValue(MotionEvent.AXIS_Z); ry = event.getAxisValue(MotionEvent.AXIS_RZ);
            leftTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
            rightTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
            keyButtons &= ~0xf0;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_X) < -.5f) keyButtons |= 0x80;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_X) > .5f) keyButtons |= 0x20;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_Y) < -.5f) keyButtons |= 0x10;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_Y) > .5f) keyButtons |= 0x40;
            sendInput(); return true;
        }
        return super.onGenericMotionEvent(event);
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused); if (!focused) clearInput();
    }
    @Override protected void onPause() { clearInput(); super.onPause(); }
    @Override public void onBackPressed() { endRuntime("Volta à garagem"); }
    private void endRuntime(String reason) {
        if (ending) return;
        clearInput(); ending = true;
        log.write("Runtime", reason);
        finish();
        // Native loader exit paths and global signal handlers are deliberately confined
        // to this process; a new game always receives fresh FEX, GPU and HLE state.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
