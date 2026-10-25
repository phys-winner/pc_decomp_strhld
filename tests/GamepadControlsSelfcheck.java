package org.stronghold.port;

import android.view.KeyEvent;
import java.util.ArrayList;
import java.util.Arrays;

public final class GamepadControlsSelfcheck {
    static final ArrayList<String> events = new ArrayList<>();
    static float movedX, movedY, panX, panY;
    static GamepadControls pad;
    static long now;
    static void check(boolean ok, String name) { if (!ok) throw new AssertionError(name + ": " + events); }
    static void expect(String name, String... expected) { check(events.toString().equals(Arrays.toString(expected)), name); events.clear(); }
    static void reset() {
        events.clear(); movedX = movedY = panX = panY = 0; now = 1000;
        pad = new GamepadControls(new GamepadControls.Output() {
            public void move(float dx, float dy) { movedX += dx; movedY += dy; }
            public void pan(float dx, float dy) { panX += dx; panY += dy; }
            public void button(int b, boolean down) { events.add("mouse:" + b + ":" + down); }
            public void key(int scan, boolean down) { events.add("key:" + scan + ":" + down); }
            public void zoom(int direction) { events.add("zoom:" + direction); }
            public void command(int key, int modifiers) { events.add("command:" + (char)key + ":" + modifiers); }
            public void action(int action) { events.add("action:" + action); }
        });
    }
    static void key(int code, boolean down) { pad.key(code, down, 0, now++); }
    static void tap(int code) { key(code, true); key(code, false); }
    static void motion(float x, float y, float z, float rz, float lt, float rt, float hx, float hy) { pad.motion(x,y,z,rz,lt,rt,hx,hy,now++); }
    public static void main(String[] args) {
        reset();
        check(GamepadControls.axis(.15f,0) == 0 && GamepadControls.axis(-.2f,.25f) == 0, "device flat and minimum deadzone");
        check(GamepadControls.axis(1,0) == 1 && GamepadControls.axis(-1,0) == -1 && GamepadControls.axis(9,0) == 1, "bounded axes");
        check(GamepadControls.axis(Float.NaN,0) == 0 && GamepadControls.axis(Float.POSITIVE_INFINITY,0) == 0, "invalid axes");
        check(!pad.key(KeyEvent.KEYCODE_VOLUME_UP,true,0,now), "system buttons stay with Android");
        key(KeyEvent.KEYCODE_BUTTON_A,true); pad.key(KeyEvent.KEYCODE_BUTTON_A,true,1,now++);
        key(KeyEvent.KEYCODE_BUTTON_B,true); key(KeyEvent.KEYCODE_BUTTON_A,false); key(KeyEvent.KEYCODE_BUTTON_B,false);
        expect("independent mouse holds and no Android repeats", "mouse:0:true","mouse:1:true","mouse:0:false","mouse:1:false");
        int[] buttons = {KeyEvent.KEYCODE_BUTTON_X,KeyEvent.KEYCODE_BUTTON_Y,KeyEvent.KEYCODE_BUTTON_C,KeyEvent.KEYCODE_BUTTON_Z,
                KeyEvent.KEYCODE_BUTTON_START,KeyEvent.KEYCODE_BUTTON_THUMBR,KeyEvent.KEYCODE_DPAD_UP,KeyEvent.KEYCODE_DPAD_DOWN,
                KeyEvent.KEYCODE_DPAD_LEFT,KeyEvent.KEYCODE_DPAD_RIGHT};
        int[] scans = {0x39,0x19,0x1C,0x0E,0x01,0x3B,0x4E,0x4A,0x2D,0x2E};
        for (int i = 0; i < buttons.length; i++) { tap(buttons[i]); expect("mapped button " + buttons[i], "key:"+scans[i]+":true","key:"+scans[i]+":false"); }
        tap(KeyEvent.KEYCODE_BUTTON_THUMBL); expect("L3 zoom reset", "action:2");
        tap(KeyEvent.KEYCODE_BUTTON_SELECT); tap(KeyEvent.KEYCODE_BUTTON_MODE); expect("short Select and Mode show layout on release", "action:1","action:1");
        reset();
        motion(0,0,0,0,0,1,0,0); key(KeyEvent.KEYCODE_BUTTON_R2,true); motion(0,0,0,0,0,.5f,0,0);
        motion(0,0,0,0,0,0,0,0); key(KeyEvent.KEYCODE_BUTTON_R2,false);
        expect("digital and analog trigger share one Shift with hysteresis", "key:42:true","key:42:false");
        motion(0,0,0,0,0,0,1,0); key(KeyEvent.KEYCODE_DPAD_RIGHT,true); motion(0,0,0,0,0,0,0,0); key(KeyEvent.KEYCODE_DPAD_RIGHT,false);
        expect("hat plus duplicate D-pad key fires once", "key:46:true","key:46:false");
        reset();
        int[] slots = {KeyEvent.KEYCODE_DPAD_UP,KeyEvent.KEYCODE_DPAD_RIGHT,KeyEvent.KEYCODE_DPAD_DOWN,KeyEvent.KEYCODE_DPAD_LEFT,
                KeyEvent.KEYCODE_BUTTON_A,KeyEvent.KEYCODE_BUTTON_B,KeyEvent.KEYCODE_BUTTON_X,KeyEvent.KEYCODE_BUTTON_Y,KeyEvent.KEYCODE_BUTTON_L1};
        for (int modifiers = 0; modifiers < 4; modifiers++) for (int i = 0; i < slots.length; i++) {
            key(KeyEvent.KEYCODE_BUTTON_SELECT,true);
            if ((modifiers & 2) != 0) key(KeyEvent.KEYCODE_BUTTON_L2,true);
            if ((modifiers & 1) != 0) key(KeyEvent.KEYCODE_BUTTON_R2,true);
            tap(slots[i]); key(KeyEvent.KEYCODE_BUTTON_SELECT,false);
            key(KeyEvent.KEYCODE_BUTTON_L2,false); key(KeyEvent.KEYCODE_BUTTON_R2,false);
            expect("all nine group/bookmark slots and save modifiers", "command:"+(char)('1'+i)+":"+modifiers);
        }
        key(KeyEvent.KEYCODE_BUTTON_SELECT,true); key(KeyEvent.KEYCODE_BUTTON_A,true); key(KeyEvent.KEYCODE_BUTTON_SELECT,false); key(KeyEvent.KEYCODE_BUTTON_A,false);
        expect("release Select before the chord key never clicks", "command:5:0");
        key(KeyEvent.KEYCODE_BUTTON_SELECT,true); tap(KeyEvent.KEYCODE_BUTTON_R1); tap(KeyEvent.KEYCODE_BUTTON_THUMBL);
        tap(KeyEvent.KEYCODE_BUTTON_THUMBR); tap(KeyEvent.KEYCODE_BUTTON_START); key(KeyEvent.KEYCODE_BUTTON_SELECT,false);
        expect("extra commands", "command:R:2","action:3","action:4","action:5");
        reset(); key(KeyEvent.KEYCODE_BUTTON_L1,true); pad.tick(now); now += 400; pad.tick(now); now += 120; pad.tick(now);
        key(KeyEvent.KEYCODE_BUTTON_L1,false); now += 1000; pad.tick(now);
        expect("zoom holds repeat and stop on release", "zoom:-1","zoom:-1","zoom:-1");
        reset(); motion(1,0,0,0,0,0,0,0); pad.tick(now); float initial = movedX;
        now += 10000; pad.tick(now); check(movedX-initial <= .048f, "stalls cap cursor displacement");
        check(pad.cursorVisible(now) && !pad.cursorVisible(now+30000) && pad.cursorVisible(now+29999), "exact 30-second idle timeout");
        reset(); motion(1,1,1,1,0,0,0,0); pad.tick(now);
        float diagonal = (float)Math.hypot(movedX,movedY);
        check(Math.abs(diagonal-initial) < .0001f && panX > 0 && panY > 0, "normalized diagonals and independent camera");
        reset(); motion(1,0,0,0,1,0,0,0); pad.tick(now); check(Math.abs(movedX-initial*.22f) < .0001f, "LT precision speed");
        reset(); pad.cancel(); motion(1,0,0,0,0,0,0,0); pad.tick(now); check(movedX > 0, "idle focus changes do not swallow the next stick movement");
        key(KeyEvent.KEYCODE_BUTTON_A,true); key(KeyEvent.KEYCODE_BUTTON_R2,true); pad.cancel();
        expect("focus/disconnect releases mouse and Shift", "mouse:0:true","key:42:true","mouse:0:false","key:42:false");
        movedX = 0; motion(1,0,0,0,0,0,0,0); pad.tick(now); check(movedX == 0 && !pad.cursorVisible(now), "held stick cannot resume after cancel");
        pad.key(KeyEvent.KEYCODE_BUTTON_A,true,1,now); expect("held key repeat cannot restart a cancelled drag");
        motion(0,0,0,0,0,0,0,0); motion(1,0,0,0,0,0,0,0); pad.tick(now); check(movedX > 0, "neutral rearms cancelled stick");
        pad.cancel(); movedX = 0; pad.reset(); motion(1,0,0,0,0,0,0,0); pad.tick(now);
        check(movedX > 0, "a new device does not inherit the old controller's held-axis gates");
        reset(); key(KeyEvent.KEYCODE_BUTTON_SELECT,true); pad.cancel(); key(KeyEvent.KEYCODE_BUTTON_SELECT,false); expect("cancelled Select never opens help");
        System.out.println("Gamepad buttons, 36 group/bookmark chords, axis calibration, precision, frame speed, zoom repeats, cancellation and 30s cursor timeout: PASS");
    }
}
