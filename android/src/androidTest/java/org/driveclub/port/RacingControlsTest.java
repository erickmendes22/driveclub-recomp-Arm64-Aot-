package org.driveclub.port;

import android.content.Intent;
import android.graphics.Rect;
import android.os.SystemClock;
import android.test.InstrumentationTestCase;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises the same View and public input callback used by GameActivity. */
public final class RacingControlsTest extends InstrumentationTestCase {
    private ControlsPreviewActivity activity;
    private RacingControls controls;
    private final AtomicReference<RacingControls.State> output = new AtomicReference<>();
    private long downTime;

    @Override protected void setUp() throws Exception {
        super.setUp();
        Intent intent = new Intent(getInstrumentation().getTargetContext(), ControlsPreviewActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        activity = (ControlsPreviewActivity) getInstrumentation().startActivitySync(intent);
        getInstrumentation().waitForIdleSync();
        getInstrumentation().runOnMainSync(() -> {
            controls = activity.getControls();
            controls.setListener(output::set);
        });
        assertTrue("The real controls must be laid out", controls.getWidth() > 0 && controls.getHeight() > 0);
        neutral();
    }

    @Override protected void tearDown() throws Exception {
        try {
            if (activity != null) getInstrumentation().runOnMainSync(() -> {
                if (controls != null) controls.releaseAll();
                activity.finish();
            });
            getInstrumentation().waitForIdleSync();
        } finally { super.tearDown(); }
    }

    public void testSteeringGasBrakeAndIndependentPointerRelease() {
        Rect steering = bounds("Direção analógica"), gas = bounds("Acelerar (R2)"), brake = bounds("Frear (L2)");
        Finger wheel = finger(7, steering), throttle = finger(19, gas), braking = finger(23, brake);
        send(MotionEvent.ACTION_DOWN, wheel);
        send(pointerDown(1), wheel, throttle);
        wheel.x += steering.width() * .16f;
        send(MotionEvent.ACTION_MOVE, wheel, throttle);
        RacingControls.State state = output.get();
        assertTrue("Steering must be fractional, not a digital D-pad", state.lx > .2f && state.lx < .85f);
        assertTrue(state.leftActive);
        assertEquals(1f, state.r2, .001f);

        send(pointerDown(2), wheel, throttle, braking);
        state = output.get();
        assertTrue(state.lx > .2f);
        assertEquals("Braking must not cancel the accelerator", 1f, state.r2, .001f);
        assertEquals(1f, state.l2, .001f);
        assertEquals(0x300, state.buttons & 0x300);

        throttle.y += gas.height() * .45f;
        send(MotionEvent.ACTION_MOVE, wheel, throttle, braking);
        state = output.get();
        assertTrue("The pedal must support modulation", state.r2 > .4f && state.r2 < .7f);
        assertEquals(1f, state.l2, .001f);
        assertTrue(state.lx > .2f);

        send(pointerUp(1), wheel, throttle, braking);
        state = output.get();
        assertEquals(0f, state.r2, .001f);
        assertEquals(1f, state.l2, .001f);
        assertTrue(state.lx > .2f);

        // Android can change pointer indices after a finger is lifted; IDs remain stable.
        wheel.x = steering.exactCenterX() - steering.width() * .16f;
        send(MotionEvent.ACTION_MOVE, braking, wheel);
        state = output.get();
        assertTrue("Pointer reordering must preserve steering ownership", state.lx < -.2f);
        assertEquals(1f, state.l2, .001f);
        assertEquals(0f, state.r2, .001f);
        send(pointerUp(0), braking, wheel);
        assertTrue(output.get().leftActive);
        assertEquals(0f, output.get().l2, .001f);
        send(MotionEvent.ACTION_UP, wheel);
        neutral();
    }

    public void testCancelAndFocusLossReleaseEveryControl() {
        Finger wheel = finger(3, bounds("Direção analógica"));
        Finger gas = finger(11, bounds("Acelerar (R2)"));
        Finger brake = finger(25, bounds("Frear (L2)"));
        send(MotionEvent.ACTION_DOWN, wheel);
        send(pointerDown(1), wheel, gas);
        send(pointerDown(2), wheel, gas, brake);
        send(MotionEvent.ACTION_CANCEL, wheel, gas, brake);
        neutral();

        send(MotionEvent.ACTION_DOWN, gas);
        assertEquals(1f, output.get().r2, .001f);
        getInstrumentation().runOnMainSync(() -> controls.onWindowFocusChanged(false));
        neutral();
        getInstrumentation().runOnMainSync(() -> controls.onWindowFocusChanged(true));
        send(MotionEvent.ACTION_MOVE, gas);
        neutral();
        send(MotionEvent.ACTION_UP, gas);
    }

    public void testMenuSwitchReleasesHeldInputsAndRoutesMenuButtons() {
        Finger gas = finger(5, bounds("Acelerar (R2)"));
        Finger wheel = finger(9, bounds("Direção analógica"));
        Finger mode = finger(16, bounds("Mostrar botões de menu"));
        send(MotionEvent.ACTION_DOWN, gas);
        send(pointerDown(1), gas, wheel);
        send(pointerDown(2), gas, wheel, mode);
        assertTrue(controls.isMenuControlsVisible());
        neutral();
        send(MotionEvent.ACTION_MOVE, gas, wheel, mode);
        neutral();
        send(pointerUp(2), gas, wheel, mode);
        send(pointerUp(1), gas, wheel);
        send(MotionEvent.ACTION_UP, gas);

        Finger cross = finger(8, bounds("Confirmar (Cruz)"));
        send(MotionEvent.ACTION_DOWN, cross);
        assertEquals(0x4000, output.get().buttons);
        assertFalse(output.get().leftActive);
        send(MotionEvent.ACTION_UP, cross);
        neutral();

        Finger up = finger(10, bounds("Direcional para cima"));
        send(MotionEvent.ACTION_DOWN, up);
        assertEquals(0x10, output.get().buttons);
        Finger race = finger(17, bounds("Mostrar controles de corrida"));
        send(pointerDown(1), up, race);
        assertFalse(controls.isMenuControlsVisible());
        neutral();
        send(pointerUp(1), up, race);
        send(MotionEvent.ACTION_UP, up);
        neutral();

        clickButton("Freio de mão (Círculo)", 0x2000);
        clickButton("Trocar câmera (R1)", 0x800);
        clickButton("Pausar (Options)", 0x8);
    }

    private void clickButton(String description, int mask) {
        Finger finger = finger(12, bounds(description));
        send(MotionEvent.ACTION_DOWN, finger);
        assertEquals(description, mask, output.get().buttons);
        send(MotionEvent.ACTION_UP, finger);
        neutral();
    }

    private void neutral() {
        RacingControls.State state = output.get();
        assertNotNull(state);
        assertEquals(0, state.buttons);
        assertEquals(0f, state.lx, .001f); assertEquals(0f, state.ly, .001f);
        assertEquals(0f, state.rx, .001f); assertEquals(0f, state.ry, .001f);
        assertEquals(0f, state.l2, .001f); assertEquals(0f, state.r2, .001f);
        assertFalse(state.leftActive); assertFalse(state.rightActive);
    }

    private Rect bounds(String description) {
        AtomicReference<Rect> result = new AtomicReference<>();
        AtomicInteger count = new AtomicInteger();
        getInstrumentation().runOnMainSync(() -> {
            List<AccessibilityNodeInfo> nodes = controls.getAccessibilityNodeProvider()
                    .findAccessibilityNodeInfosByText(description, -1);
            count.set(nodes.size());
            if (nodes.size() == 1) {
                Rect rect = new Rect(); nodes.get(0).getBoundsInParent(rect);
                result.set(rect);
            }
            for (AccessibilityNodeInfo node : nodes) node.recycle();
        });
        assertEquals("One visible control must match: " + description, 1, count.get());
        assertTrue("Control must have touch bounds", result.get().width() > 0 && result.get().height() > 0);
        return result.get();
    }

    private static final class Finger {
        final int id;
        float x, y;
        Finger(int id, float x, float y) { this.id = id; this.x = x; this.y = y; }
    }
    private static Finger finger(int id, Rect bounds) { return new Finger(id, bounds.exactCenterX(), bounds.exactCenterY()); }
    private static int pointerDown(int index) { return MotionEvent.ACTION_POINTER_DOWN | index << MotionEvent.ACTION_POINTER_INDEX_SHIFT; }
    private static int pointerUp(int index) { return MotionEvent.ACTION_POINTER_UP | index << MotionEvent.ACTION_POINTER_INDEX_SHIFT; }

    private void send(int action, Finger... fingers) {
        long now = SystemClock.uptimeMillis();
        if (action == MotionEvent.ACTION_DOWN) downTime = now;
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[fingers.length];
        MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[fingers.length];
        for (int i=0;i<fingers.length;++i) {
            properties[i] = new MotionEvent.PointerProperties();
            properties[i].id = fingers[i].id; properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coordinates[i] = new MotionEvent.PointerCoords();
            coordinates[i].x = fingers[i].x; coordinates[i].y = fingers[i].y;
            coordinates[i].pressure = 1; coordinates[i].size = 1;
        }
        MotionEvent event = MotionEvent.obtain(downTime, now, action, fingers.length, properties, coordinates,
                0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        AtomicBoolean consumed = new AtomicBoolean();
        try {
            getInstrumentation().runOnMainSync(() -> consumed.set(controls.dispatchTouchEvent(event)));
        } finally { event.recycle(); }
        assertTrue("The view must consume the touch stream", consumed.get());
    }
}
