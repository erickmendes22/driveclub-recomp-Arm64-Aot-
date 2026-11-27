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
    private RacingControls controls;
    private RacingControls.State touch = RacingControls.State.neutral();
    private int keyButtons, hatButtons;
    private float lx, ly, rx, ry, leftTrigger, rightTrigger;
    private boolean analogLeftTrigger, analogRightTrigger;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        log = new PortLog(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setDecorFitsSystemWindows(false);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(-1, -1));

        controls = new RacingControls(this);
        controls.setListener(input -> { touch = input; sendInput(); });
        root.addView(controls, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        Button close = button("Sair", "Voltar à garagem");
        close.setOnClickListener(v -> endRuntime("Encerrado pelo usuário"));
        top.addView(close);
        TextView hint = new TextView(this);
        hint.setText("Driveclub"); hint.setTextColor(0xccffffff); hint.setPadding(dp(12), 0, 0, 0);
        top.addView(hint);
        FrameLayout.LayoutParams topParams = new FrameLayout.LayoutParams(-2, dp(48), Gravity.TOP | Gravity.START);
        topParams.topMargin = dp(8); topParams.leftMargin = dp(12);
        root.addView(top, topParams);

        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets cutout = insets.getInsets(WindowInsets.Type.displayCutout());
            v.setPadding(cutout.left, cutout.top, cutout.right, cutout.bottom);
            return insets;
        });
        setContentView(root);
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.hide(WindowInsets.Type.systemBars());
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
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
    private Button button(String label, String description) {
        Button button = new Button(this);
        button.setText(label); button.setTextSize(18); button.setAllCaps(false);
        button.setContentDescription(description); button.setTextColor(Color.WHITE);
        button.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0x77202128));
        button.setMinWidth(0); button.setMinimumWidth(0); button.setPadding(0, 0, 0, 0);
        button.setLayoutParams(new LinearLayout.LayoutParams(dp(52), dp(48)));
        return button;
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
                String update = getIntent().getStringExtra("update");
                files = new RuntimeFiles(this, Uri.parse(getIntent().getStringExtra("base")),
                        update == null ? null : Uri.parse(update));
                DriverManager.configure(this, log);
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
        int buttons = keyButtons | hatButtons | touch.buttons;
        // Digital trigger keys are a fallback only for pads without trigger axes.
        // Touch trigger bits must never turn a fractional pedal value into full throttle.
        float padL2 = Math.max(leftTrigger, !analogLeftTrigger && (keyButtons & 0x100) != 0 ? 1 : 0);
        float padR2 = Math.max(rightTrigger, !analogRightTrigger && (keyButtons & 0x200) != 0 ? 1 : 0);
        inputNative(buttons, touch.leftActive ? touch.lx : lx, touch.leftActive ? touch.ly : ly,
                touch.rightActive ? touch.rx : rx, touch.rightActive ? touch.ry : ry,
                Math.max(padL2, touch.l2), Math.max(padR2, touch.r2));
    }
    private void clearInput() {
        keyButtons = hatButtons = 0;
        lx = ly = rx = ry = leftTrigger = rightTrigger = 0;
        touch = RacingControls.State.neutral();
        if (controls != null) controls.releaseAll();
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
            updateTriggerCapabilities(event.getDevice());
            if (event.getAction() == KeyEvent.ACTION_DOWN) keyButtons |= mask;
            else keyButtons &= ~mask;
            sendInput(); return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (event.isFromSource(InputDevice.SOURCE_JOYSTICK) && event.getAction() == MotionEvent.ACTION_MOVE) {
            updateTriggerCapabilities(event.getDevice());
            lx = stickAxis(event, MotionEvent.AXIS_X); ly = stickAxis(event, MotionEvent.AXIS_Y);
            rx = stickAxis(event, MotionEvent.AXIS_Z); ry = stickAxis(event, MotionEvent.AXIS_RZ);
            leftTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
            rightTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
            hatButtons = 0;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_X) < -.5f) hatButtons |= 0x80;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_X) > .5f) hatButtons |= 0x20;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_Y) < -.5f) hatButtons |= 0x10;
            if (event.getAxisValue(MotionEvent.AXIS_HAT_Y) > .5f) hatButtons |= 0x40;
            sendInput(); return true;
        }
        return super.onGenericMotionEvent(event);
    }
    private void updateTriggerCapabilities(InputDevice device) {
        if (device == null) return;
        analogLeftTrigger = device.getMotionRange(MotionEvent.AXIS_LTRIGGER) != null
                || device.getMotionRange(MotionEvent.AXIS_BRAKE) != null;
        analogRightTrigger = device.getMotionRange(MotionEvent.AXIS_RTRIGGER) != null
                || device.getMotionRange(MotionEvent.AXIS_GAS) != null;
    }
    private float stickAxis(MotionEvent event, int axis) {
        InputDevice device = event.getDevice();
        InputDevice.MotionRange range = device == null ? null : device.getMotionRange(axis, event.getSource());
        float value = event.getAxisValue(axis), flat = range == null ? .08f : Math.max(.04f, range.getFlat());
        return Math.abs(value) <= flat ? 0 : Math.max(-1, Math.min(1, value));
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
