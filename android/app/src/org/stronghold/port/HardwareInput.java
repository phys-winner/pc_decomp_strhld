package org.stronghold.port;

import android.view.KeyEvent;

/** USB/Bluetooth keyboard and mouse, independent of Android so the key table
 * and the mouse arbitration can be tested on a PC. A mouse drives the original
 * desktop path; only the wheel and the middle button get port meanings. */
final class HardwareInput {
    /* Android button bits (MotionEvent.BUTTON_PRIMARY/SECONDARY/TERTIARY). */
    static final int LEFT = 1, RIGHT = 2, MIDDLE = 4;
    /* One wheel notch zooms by this factor; a pause this long starts a new
     * wheel burst, which may settle on 100% again (the pinch detent). */
    static final float ZOOM_STEP = 1.15f;
    static final long WHEEL_BURST_MS = 400;
    static final int WHEEL_DELTA = 120;
    private static final int SOURCE_MOUSE = 0x2002, TOOL_TYPE_MOUSE = 3; /* InputDevice / MotionEvent */
    private static final byte[] LETTERS = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
            0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    private static final byte[] KEYPAD = {0x52,0x4F,0x50,0x51,0x4B,0x4C,0x4D,0x47,0x48,0x49};

    static boolean isMouse(int source, int toolType) {
        return (source & SOURCE_MOUSE) == SOURCE_MOUSE || toolType == TOOL_TYPE_MOUSE;
    }

    /** Edge-scroll bits (1 right, 2 left; input.h) for a pointer that left the
     * game view at x: system edge-gesture windows (MIUI) take a mouse before
     * it reaches the screen edge, so leaving through the outer strip counts. */
    static int exitEdges(float x, float width) {
        return width <= 0 ? 0 : x <= width * .12f ? 2 : x >= width * .88f ? 1 : 0;
    }

    /** The DirectInput scan code of an Android key, 0 for keys the game never
     * had (volume, media, system keys stay with Android). */
    static int scanCode(int key) {
        if (key >= KeyEvent.KEYCODE_A && key <= KeyEvent.KEYCODE_Z) return LETTERS[key - KeyEvent.KEYCODE_A];
        if (key >= KeyEvent.KEYCODE_1 && key <= KeyEvent.KEYCODE_9) return 0x02 + key - KeyEvent.KEYCODE_1;
        if (key >= KeyEvent.KEYCODE_F1 && key <= KeyEvent.KEYCODE_F10) return 0x3B + key - KeyEvent.KEYCODE_F1;
        if (key >= KeyEvent.KEYCODE_NUMPAD_0 && key <= KeyEvent.KEYCODE_NUMPAD_9) return KEYPAD[key - KeyEvent.KEYCODE_NUMPAD_0];
        switch (key) {
            case KeyEvent.KEYCODE_0: return 0x0B;
            case KeyEvent.KEYCODE_F11: return 0x57;
            case KeyEvent.KEYCODE_F12: return 0x58;
            case KeyEvent.KEYCODE_ESCAPE: return 0x01;
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER: return 0x1C;
            case KeyEvent.KEYCODE_SPACE: return 0x39;
            case KeyEvent.KEYCODE_TAB: return 0x0F;
            case KeyEvent.KEYCODE_DEL: return 0x0E;            /* Backspace */
            case KeyEvent.KEYCODE_FORWARD_DEL: return 0xD3;
            case KeyEvent.KEYCODE_INSERT: return 0xD2;
            case KeyEvent.KEYCODE_MOVE_HOME: return 0xC7;
            case KeyEvent.KEYCODE_MOVE_END: return 0xCF;
            case KeyEvent.KEYCODE_PAGE_UP: return 0xC9;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 0xD1;
            case KeyEvent.KEYCODE_DPAD_UP: return 0xC8;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0xD0;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0xCB;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0xCD;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 0x2A;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0x36;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 0x1D;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0x9D;
            case KeyEvent.KEYCODE_ALT_LEFT: return 0x38;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xB8;
            case KeyEvent.KEYCODE_MINUS: return 0x0C;
            case KeyEvent.KEYCODE_EQUALS: return 0x0D;
            case KeyEvent.KEYCODE_LEFT_BRACKET: return 0x1A;
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return 0x1B;
            case KeyEvent.KEYCODE_BACKSLASH: return 0x2B;
            case KeyEvent.KEYCODE_SEMICOLON: return 0x27;
            case KeyEvent.KEYCODE_APOSTROPHE: return 0x28;
            case KeyEvent.KEYCODE_GRAVE: return 0x29;
            case KeyEvent.KEYCODE_COMMA: return 0x33;
            case KeyEvent.KEYCODE_PERIOD: return 0x34;
            case KeyEvent.KEYCODE_SLASH: return 0x35;
            case KeyEvent.KEYCODE_NUMPAD_ADD: return 0x4E;
            case KeyEvent.KEYCODE_NUMPAD_SUBTRACT: return 0x4A;
            case KeyEvent.KEYCODE_NUMPAD_MULTIPLY: return 0x37;
            case KeyEvent.KEYCODE_NUMPAD_DIVIDE: return 0xB5;
            case KeyEvent.KEYCODE_NUMPAD_DOT: return 0x53;
            case KeyEvent.KEYCODE_BREAK: return 0xC5;          /* Pause */
            case KeyEvent.KEYCODE_CAPS_LOCK: return 0x3A;
            case KeyEvent.KEYCODE_NUM_LOCK: return 0x45;
            case KeyEvent.KEYCODE_SCROLL_LOCK: return 0x46;
            default: return 0;
        }
    }

    interface Output {
        /** Guest button 0 left, 1 right. */
        void button(int button, boolean down);
        /** The cursor at a view point; warp puts it there without mouse motion. */
        void move(float x, float y, boolean warp);
        /** delta in original WHEEL_DELTA units (may be 0 for a fine wheel), zoom the matching factor. */
        void wheel(float x, float y, int delta, float zoom, boolean burst);
        void grab(float x, float y, float dx, float dy);
        void middleClick(float x, float y);
    }

    /** One physical mouse: absolute cursor, left/right as the original
     * buttons, wheel, and the middle button for dragging the map (a click
     * without movement returns the zoom to 100%). */
    static final class Mouse {
        private final Output out;
        private final float slop;
        private int held, ignored;
        private boolean placed, grabbing, grabbed;
        private float lastX, lastY, grabX, grabY, wheelRest;
        private long wheelAt = Long.MIN_VALUE / 2;

        Mouse(Output output, float touchSlop) { out = output; slop = touchSlop; }
        int held() { return held; }

        /** A hover, drag or button event with Android's current button state. */
        void pointer(float x, float y, int state) {
            state &= LEFT | RIGHT | MIDDLE;
            ignored &= state; /* a button held through a cancel counts again once released */
            int next = state & ~ignored;
            if (grabbing) {
                if (!grabbed && Math.hypot(x - grabX, y - grabY) > slop) { grabbed = true; out.grab(x, y, x - grabX, y - grabY); }
                else if (grabbed && (x != lastX || y != lastY)) out.grab(x, y, x - lastX, y - lastY);
            }
            if (!placed || x != lastX || y != lastY) { out.move(x, y, !placed); placed = true; }
            lastX = x; lastY = y;
            for (int i = 0; i < 2; i++) if (((held ^ next) & (1 << i)) != 0) out.button(i, (next & (1 << i)) != 0);
            if ((next & ~held & MIDDLE) != 0) { grabbing = true; grabbed = false; grabX = x; grabY = y; }
            else if ((held & ~next & MIDDLE) != 0) { if (grabbing && !grabbed) out.middleClick(x, y); grabbing = false; }
            held = next;
        }

        /** Vertical wheel in notches, positive away from the user. The original
         * acts on whole notches only (one step each); a fine wheel's fractions
         * add up to them, while the zoom follows every fraction. */
        void scroll(float x, float y, float notches, long time) {
            if (notches == 0 || Float.isNaN(notches) || Float.isInfinite(notches)) return;
            boolean burst = time - wheelAt > WHEEL_BURST_MS;
            wheelAt = time;
            if (burst || Math.signum(wheelRest) == -Math.signum(notches)) wheelRest = 0;
            if (!placed || x != lastX || y != lastY) { out.move(x, y, !placed); placed = true; lastX = x; lastY = y; }
            wheelRest += notches;
            int whole = (int)wheelRest;
            wheelRest -= whole;
            out.wheel(x, y, whole * WHEEL_DELTA, (float)Math.pow(ZOOM_STEP, notches), burst);
        }

        /** Focus loss, an Android cancel or a finger taking over: release
         * everything. Buttons still held stay ignored until let go, so a press
         * never restarts in the middle of a stale drag. */
        void cancel() {
            for (int i = 0; i < 2; i++) if ((held & (1 << i)) != 0) out.button(i, false);
            ignored |= held; held = 0;
            grabbing = grabbed = placed = false;
            wheelRest = 0; wheelAt = Long.MIN_VALUE / 2;
        }
    }
}
