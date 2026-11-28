package org.driveclub.port;

import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.test.InstrumentationTestCase;
import android.view.WindowManager;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Uses a real Android window and the helper called by the game's JNI bridge. */
public final class ScreenshotPolicyTest extends InstrumentationTestCase {
    private ControlsPreviewActivity activity;
    private final AtomicBoolean ending = new AtomicBoolean();
    private ScreenshotPolicy policy;

    @Override protected void setUp() throws Exception {
        super.setUp();
        Intent intent = new Intent(getInstrumentation().getTargetContext(), ControlsPreviewActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        activity = (ControlsPreviewActivity) getInstrumentation().startActivitySync(intent);
        getInstrumentation().waitForIdleSync();
        policy = new ScreenshotPolicy(activity, ending::get);
    }

    @Override protected void tearDown() throws Exception {
        try {
            if (activity != null) getInstrumentation().runOnMainSync(() -> {
                activity.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_SECURE);
                activity.finish();
            });
            getInstrumentation().waitForIdleSync();
        } finally { super.tearDown(); }
    }

    public void testMainThreadPolicyAndEndingRejection() {
        getInstrumentation().runOnMainSync(() -> {
            assertTrue(policy.apply(false));
            assertTrue(secure());
            assertTrue(policy.apply(true));
            assertFalse(secure());
            ending.set(true);
            assertFalse("An ending activity cannot acknowledge a change", policy.apply(false));
            assertFalse(secure());
        });
    }

    public void testWorkerAcknowledgesAppliedWindowFlags() {
        assertNotSame("The JNI-style caller must be outside the UI thread",
                      Looper.getMainLooper(), Looper.myLooper());
        assertTrue(policy.apply(false));
        getInstrumentation().runOnMainSync(() -> assertTrue(secure()));
        assertTrue(policy.apply(true));
        getInstrumentation().runOnMainSync(() -> assertFalse(secure()));
        ending.set(true);
        assertFalse(policy.apply(false));
        getInstrumentation().runOnMainSync(() -> assertFalse(secure()));
    }

    public void testQueuedTimeoutCannotApplyLater() throws Exception {
        ScreenshotPolicy shortWait = new ScreenshotPolicy(activity, ending::get, 100);
        CountDownLatch uiEntered = new CountDownLatch(1);
        CountDownLatch releaseUi = new CountDownLatch(1);
        AtomicBoolean released = new AtomicBoolean();
        getInstrumentation().runOnMainSync(() -> assertFalse(secure()));
        new Handler(Looper.getMainLooper()).post(() -> {
            uiEntered.countDown();
            try { released.set(releaseUi.await(3, TimeUnit.SECONDS)); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        });
        try {
            assertTrue("The UI queue must be blocked before requesting the policy",
                       uiEntered.await(2, TimeUnit.SECONDS));
            assertFalse("A queued request must fail when its UI deadline expires", shortWait.apply(false));
        } finally { releaseUi.countDown(); }
        getInstrumentation().waitForIdleSync();
        assertTrue("The test releases the UI explicitly, without waiting for its safety deadline", released.get());
        getInstrumentation().runOnMainSync(() -> assertFalse(
                "The timed-out request cannot later change the window", secure()));
    }

    private boolean secure() {
        return (activity.getWindow().getAttributes().flags & WindowManager.LayoutParams.FLAG_SECURE) != 0;
    }
}
