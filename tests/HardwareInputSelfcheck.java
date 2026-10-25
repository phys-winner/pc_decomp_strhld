package org.stronghold.port;

import android.view.KeyEvent;
import java.util.ArrayList;
import java.util.HashMap;

public final class HardwareInputSelfcheck {
    static final ArrayList<String> events = new ArrayList<>();
    static HardwareInput.Mouse mouse;
    static final int LEFT = HardwareInput.LEFT, RIGHT = HardwareInput.RIGHT, MIDDLE = HardwareInput.MIDDLE;
    static void check(boolean ok, String name) { if (!ok) throw new AssertionError(name + ": " + events); }
    static void expect(String name, String... expected) {
        check(events.toString().equals(java.util.Arrays.toString(expected)), name); events.clear();
    }
    static void reset() {
        events.clear();
        mouse = new HardwareInput.Mouse(new HardwareInput.Output() {
            public void button(int b, boolean down) { events.add("button:" + b + ":" + down); }
            public void move(float x, float y, boolean warp) { events.add((warp ? "warp:" : "move:") + (int)x + ":" + (int)y); }
            public void wheel(float x, float y, int delta, float zoom, boolean burst) {
                events.add("wheel:" + delta + ":" + Math.round(zoom * 1000) + (burst ? ":burst" : ""));
            }
            public void grab(float x, float y, float dx, float dy) { events.add("grab:" + (int)dx + ":" + (int)dy); }
            public void middleClick(float x, float y) { events.add("middle:" + (int)x + ":" + (int)y); }
        }, 8);
    }
    static int scan(int key) { return HardwareInput.scanCode(key); }
    public static void main(String[] args) {
        /* Keyboard: Android key codes become the game's DirectInput scan codes. */
        check(scan(KeyEvent.KEYCODE_A) == 0x1E && scan(KeyEvent.KEYCODE_Q) == 0x10 && scan(KeyEvent.KEYCODE_Z) == 0x2C, "letters");
        check(scan(KeyEvent.KEYCODE_1) == 0x02 && scan(KeyEvent.KEYCODE_9) == 0x0A && scan(KeyEvent.KEYCODE_0) == 0x0B, "digit row");
        check(scan(KeyEvent.KEYCODE_F1) == 0x3B && scan(KeyEvent.KEYCODE_F10) == 0x44 &&
              scan(KeyEvent.KEYCODE_F11) == 0x57 && scan(KeyEvent.KEYCODE_F12) == 0x58, "function keys");
        check(scan(KeyEvent.KEYCODE_ESCAPE) == 0x01 && scan(KeyEvent.KEYCODE_ENTER) == 0x1C && scan(KeyEvent.KEYCODE_NUMPAD_ENTER) == 0x1C &&
              scan(KeyEvent.KEYCODE_SPACE) == 0x39 && scan(KeyEvent.KEYCODE_TAB) == 0x0F && scan(KeyEvent.KEYCODE_DEL) == 0x0E &&
              scan(KeyEvent.KEYCODE_FORWARD_DEL) == 0xD3, "editing keys");
        check(scan(KeyEvent.KEYCODE_DPAD_LEFT) == 0xCB && scan(KeyEvent.KEYCODE_DPAD_RIGHT) == 0xCD &&
              scan(KeyEvent.KEYCODE_DPAD_UP) == 0xC8 && scan(KeyEvent.KEYCODE_DPAD_DOWN) == 0xD0 &&
              scan(KeyEvent.KEYCODE_PAGE_UP) == 0xC9 && scan(KeyEvent.KEYCODE_MOVE_HOME) == 0xC7, "navigation keys");
        check(scan(KeyEvent.KEYCODE_NUMPAD_ADD) == 0x4E && scan(KeyEvent.KEYCODE_NUMPAD_SUBTRACT) == 0x4A &&
              scan(KeyEvent.KEYCODE_NUMPAD_0) == 0x52 && scan(KeyEvent.KEYCODE_NUMPAD_7) == 0x47 &&
              scan(KeyEvent.KEYCODE_NUMPAD_MULTIPLY) == 0x37 && scan(KeyEvent.KEYCODE_NUMPAD_DOT) == 0x53, "numeric pad");
        check(scan(KeyEvent.KEYCODE_CTRL_LEFT) == 0x1D && scan(KeyEvent.KEYCODE_CTRL_RIGHT) == 0x9D &&
              scan(KeyEvent.KEYCODE_SHIFT_LEFT) == 0x2A && scan(KeyEvent.KEYCODE_SHIFT_RIGHT) == 0x36 &&
              scan(KeyEvent.KEYCODE_ALT_LEFT) == 0x38 && scan(KeyEvent.KEYCODE_ALT_RIGHT) == 0xB8, "modifiers");
        for (int key : new int[] {KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE,
                KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_POWER, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
                KeyEvent.KEYCODE_META_LEFT, KeyEvent.KEYCODE_SYSRQ, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_UNKNOWN})
            check(scan(key) == 0, "system key " + key + " stays with Android");
        HashMap<Integer, Integer> owners = new HashMap<>();
        for (int key = 0; key < 1024; key++) {
            int code = scan(key); if (code == 0) continue;
            Integer other = owners.put(code, key);
            check(other == null || (code == 0x1C && key == KeyEvent.KEYCODE_NUMPAD_ENTER), "one Android key per scan code " + code);
        }
        check(owners.size() == 99, "full keyboard coverage " + owners.size());
        check(HardwareInput.exitEdges(10, 2340) == 2 && HardwareInput.exitEdges(2330, 2340) == 1 &&
              HardwareInput.exitEdges(1170, 2340) == 0 && HardwareInput.exitEdges(10, 0) == 0, "leaving towards a side edge keeps scrolling");
        check(HardwareInput.isMouse(0x2002, 3) && HardwareInput.isMouse(0x2002, 1) && HardwareInput.isMouse(0, 3) &&
              !HardwareInput.isMouse(0x1002, 1) && !HardwareInput.isMouse(0x4002, 2) && !HardwareInput.isMouse(0x5002, 2), "mouse sources");

        /* Mouse: the cursor is placed without motion first, then follows. */
        reset(); mouse.pointer(100, 100, 0); mouse.pointer(110, 100, 0); mouse.pointer(110, 100, 0);
        expect("first position warps, later ones move, still pointer is silent", "warp:100:100", "move:110:100");
        mouse.pointer(110, 100, LEFT); mouse.pointer(120, 100, LEFT); mouse.pointer(120, 100, 0);
        expect("left press, drag and release in order", "button:0:true", "move:120:100", "button:0:false");
        mouse.pointer(130, 100, RIGHT); mouse.pointer(130, 100, 0);
        expect("right button is the game's own", "move:130:100", "button:1:true", "button:1:false");
        mouse.pointer(130, 100, LEFT | RIGHT); mouse.pointer(130, 100, RIGHT); mouse.pointer(130, 100, 0);
        expect("chorded buttons", "button:0:true", "button:1:true", "button:0:false", "button:1:false");
        mouse.pointer(130, 100, MIDDLE); mouse.pointer(133, 102, MIDDLE); mouse.pointer(133, 102, 0);
        expect("a still middle click returns the zoom, no guest button", "move:133:102", "middle:133:102");
        mouse.pointer(133, 102, MIDDLE); mouse.pointer(151, 102, MIDDLE); mouse.pointer(161, 112, MIDDLE); mouse.pointer(161, 112, 0);
        expect("middle drag grabs the map from its first point", "grab:18:0", "move:151:102", "grab:10:10", "move:161:112");
        mouse.pointer(161, 112, MIDDLE | LEFT); mouse.pointer(161, 112, 0);
        expect("left with middle still clicks", "button:0:true", "button:0:false", "middle:161:112");

        /* Wheel: bursts re-arm the 100% detent; the original sees whole notches. */
        mouse.scroll(161, 112, 1, 1000); mouse.scroll(161, 112, 1, 1100); mouse.scroll(161, 112, -1, 2000);
        expect("notches, zoom factor and bursts", "wheel:120:1150:burst", "wheel:120:1150", "wheel:-120:870:burst");
        for (int i = 0; i < 4; i++) mouse.scroll(161, 112, .25f, 3000 + 10 * i);
        expect("a fine wheel adds up to one original notch", "wheel:0:1036:burst", "wheel:0:1036", "wheel:0:1036", "wheel:120:1036");
        mouse.scroll(161, 112, .5f, 3100); mouse.scroll(161, 112, -.75f, 3110); mouse.scroll(161, 112, -.75f, 3120);
        expect("a reversed fine wheel starts its own notch", "wheel:0:1072", "wheel:0:900", "wheel:-120:900");
        mouse.scroll(200, 150, 1, 9000);
        expect("the wheel moves the cursor to where it turns", "move:200:150", "wheel:120:1150:burst");

        /* Cancel releases what the game holds; a still-held button stays ignored until let go. */
        mouse.pointer(200, 150, LEFT); mouse.cancel(); mouse.pointer(210, 150, LEFT); mouse.pointer(210, 150, 0); mouse.pointer(215, 150, LEFT);
        expect("cancel releases, no re-press mid-drag", "button:0:true", "button:0:false", "warp:210:150", "move:215:150", "button:0:true");
        mouse.cancel(); mouse.pointer(215, 150, 0); mouse.pointer(215, 150, MIDDLE); mouse.pointer(240, 150, MIDDLE); mouse.cancel();
        mouse.pointer(240, 150, MIDDLE); mouse.pointer(260, 150, MIDDLE); mouse.pointer(260, 150, 0);
        expect("a cancelled grab neither pans on nor clicks", "button:0:false", "warp:215:150", "grab:25:0", "move:240:150",
               "warp:240:150", "move:260:150");
        check(mouse.held() == 0, "nothing held");
        System.out.println("HardwareInput keyboard scan codes, mouse sources, edge exits, buttons, wheel notches/bursts, middle grab/click and cancellation: PASS");
    }
}
