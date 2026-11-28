package org.driveclub.port;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.view.Window;
import android.view.WindowManager;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.BooleanSupplier;

/** Applies capture permission to the actual Android window, without capturing an image. */
final class ScreenshotPolicy {
    private final Activity activity;
    private final BooleanSupplier ending;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final long timeoutMillis;

    ScreenshotPolicy(Activity activity, BooleanSupplier ending) {
        this(activity, ending, 2000);
    }

    ScreenshotPolicy(Activity activity, BooleanSupplier ending, long timeoutMillis) {
        if (timeoutMillis <= 0) throw new IllegalArgumentException("Positive UI timeout required");
        this.activity = activity;
        this.ending = ending;
        this.timeoutMillis = timeoutMillis;
    }

    boolean apply(boolean enabled) {
        if (Looper.myLooper() == ui.getLooper()) return applyOnUi(enabled);
        Change change = new Change(enabled);
        FutureTask<Boolean> task = new FutureTask<>(change::run);
        if (!ui.post(task)) return false;
        try {
            return task.get(timeoutMillis, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            boolean result = change.cancelOrResult(task);
            ui.removeCallbacks(task);
            Thread.currentThread().interrupt();
            return result;
        } catch (TimeoutException | ExecutionException | CancellationException e) {
            boolean result = change.cancelOrResult(task);
            ui.removeCallbacks(task);
            return result;
        }
    }

    private boolean applyOnUi(boolean enabled) {
        try {
            if (ending.getAsBoolean() || activity.isFinishing() || activity.isDestroyed()) return false;
            Window window = activity.getWindow();
            if (window == null) return false;
            // Android also applies FLAG_SECURE to non-secure displays and mirroring.
            // SurfaceView.setSecure cannot be toggled here: it must precede attachment.
            if (enabled) window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE);
            else window.addFlags(WindowManager.LayoutParams.FLAG_SECURE);
            boolean secure = (window.getAttributes().flags & WindowManager.LayoutParams.FLAG_SECURE) != 0;
            return secure != enabled;
        } catch (RuntimeException e) {
            return false;
        }
    }

    private final class Change {
        private final boolean enabled;
        private boolean cancelled, completed, result;

        Change(boolean enabled) { this.enabled = enabled; }

        synchronized boolean run() {
            if (cancelled) return false;
            result = applyOnUi(enabled);
            completed = true;
            return result;
        }

        synchronized boolean cancelOrResult(FutureTask<Boolean> task) {
            // Serialize cancellation with the flag change itself. A queued change
            // cannot act after a timeout. If the UI already applied it, acknowledge
            // that result instead of returning failure before a late side effect.
            // The worker never holds this monitor while waiting for the UI task.
            if (completed) return result;
            cancelled = true;
            task.cancel(false);
            return false;
        }
    }
}
