package org.driveclub.port;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.Locale;

/** The same live controls as the game, without starting native code or opening game files. */
public final class ControlsPreviewActivity extends Activity {
    private RacingControls controls;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setDecorFitsSystemWindows(false);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xff10151d);
        ImageView backdrop = new ImageView(this);
        backdrop.setImageResource(R.drawable.garage_cover);
        backdrop.setScaleType(ImageView.ScaleType.CENTER_CROP);
        backdrop.setColorFilter(0xb510151d);
        backdrop.setImportantForAccessibility(android.view.View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        root.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout readout = new LinearLayout(this);
        readout.setOrientation(LinearLayout.VERTICAL);
        readout.setGravity(Gravity.CENTER);
        TextView heading = text("TESTE DE CONTROLES", 17, 0xfff4f6fa);
        heading.setTypeface(android.graphics.Typeface.create("sans-serif-medium", 0));
        readout.addView(heading);
        readout.addView(text("Sem iniciar o jogo · Direção e pedais podem ser usados juntos", 12, 0xffbfc7d4));
        TextView values = text("", 15, 0xfff4f6fa);
        values.setPadding(0, dp(10), 0, 0);
        values.setTypeface(android.graphics.Typeface.MONOSPACE);
        readout.addView(values);
        TextView instructions = text("Arraste o analógico para virar. Segure R2/L2 e deslize para baixo para dosar.\n"
                + "MENU mostra os botões usados para navegar no jogo.", 12, 0xffbfc7d4);
        instructions.setPadding(dp(8), dp(9), dp(8), 0);
        readout.addView(instructions);
        FrameLayout.LayoutParams readoutParams = new FrameLayout.LayoutParams(-1, -2, Gravity.TOP);
        readoutParams.topMargin = dp(65);
        root.addView(readout, readoutParams);

        controls = new RacingControls(this);
        controls.setListener(input -> values.setText(String.format(Locale.ROOT,
                "Direção %+04.0f%%   Acelerador %3.0f%%   Freio %3.0f%%\n%s",
                input.lx * 100, input.r2 * 100, input.l2 * 100, pressed(input.buttons))));
        root.addView(controls, new FrameLayout.LayoutParams(-1, -1));

        Button back = new Button(this);
        back.setText("Voltar"); back.setAllCaps(false); back.setTextColor(Color.WHITE);
        back.setContentDescription("Voltar à garagem");
        back.setBackgroundTintList(ColorStateList.valueOf(0x88232c38));
        back.setOnClickListener(v -> finish());
        FrameLayout.LayoutParams backParams = new FrameLayout.LayoutParams(dp(84), dp(48), Gravity.TOP | Gravity.START);
        backParams.setMargins(dp(12), dp(8), 0, 0);
        root.addView(back, backParams);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets cutout = insets.getInsets(WindowInsets.Type.displayCutout());
            v.setPadding(cutout.left, cutout.top, cutout.right, cutout.bottom);
            return insets;
        });
        setContentView(root);
        // PhoneWindow creates its DecorView during setContentView. Querying the
        // insets controller before that throws on Android 15.
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.hide(WindowInsets.Type.systemBars());
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    /** Public view surface for the preview and Android MotionEvent integration tests. */
    public RacingControls getControls() { return controls; }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this);
        view.setText(value); view.setTextSize(size); view.setTextColor(color); view.setGravity(Gravity.CENTER);
        return view;
    }
    private static String pressed(int buttons) {
        if (buttons == 0) return "Nenhum botão pressionado";
        StringBuilder names = new StringBuilder();
        int[] bits = {0x8,0x10,0x20,0x40,0x80,0x100,0x200,0x400,0x800,0x1000,0x2000,0x4000,0x8000,0x100000};
        String[] labels = {"Options","↑","→","↓","←","L2","R2","L1","R1","△","○","×","□","Touchpad"};
        for (int i=0;i<bits.length;++i) if ((buttons & bits[i]) != 0) {
            if (names.length()>0) names.append(" · ");
            names.append(labels[i]);
        }
        return names.toString();
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (!focused && controls != null) controls.releaseAll();
    }
    @Override protected void onPause() {
        if (controls != null) controls.releaseAll();
        super.onPause();
    }
}
