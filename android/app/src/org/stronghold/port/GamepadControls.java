package org.stronghold.port;

import android.view.KeyEvent;

/** Controller state and timing, kept independent of Android event delivery. */
final class GamepadControls {
    static final long CURSOR_IDLE_MS = 30000;
    static final int HELP = 1, RESET_ZOOM = 2, TOOLS = 3, KEYBOARD = 4, SETTINGS = 5;
    private static final int[] KEYS = {96,97,98,99,100,101,102,103,104,105,106,107,108,109,19,20,21,22};
    interface Output {
        void move(float dx, float dy);
        void pan(float dx, float dy);
        void button(int button, boolean down);
        void key(int scan, boolean down);
        void zoom(int direction);
        void command(int key, int modifiers);
        void action(int action);
    }
    private final Output out;
    private long keys, axes, state, normal, suppressed, frameAt = -1, repeatAt, activeAt = -1;
    private float lx, ly, rx, ry;
    private boolean selectUsed, blockLeft, blockRight, blockTriggers, blockHat;

    GamepadControls(Output output) { out = output; }
    private static long bit(int key) {
        if (key == KeyEvent.KEYCODE_BUTTON_MODE || key == KeyEvent.KEYCODE_BACK) key = KeyEvent.KEYCODE_BUTTON_SELECT;
        for (int i = 0; i < KEYS.length; i++) if (KEYS[i] == key) return 1L << i;
        return 0;
    }
    static boolean handles(int key) { return bit(key) != 0; }
    private static boolean has(long bits, int key) { return (bits & bit(key)) != 0; }
    static float axis(float value, float flat) {
        if (!Float.isFinite(value) || !Float.isFinite(flat)) return 0;
        float dead = Math.max(.16f, Math.min(.9f, flat));
        return Math.abs(value) <= dead ? 0 : Math.copySign(Math.min(1, (Math.abs(value) - dead) / (1 - dead)), value);
    }
    boolean key(int key, boolean down, int repeat, long now) {
        long mask = bit(key);
        if (mask == 0) return false;
        if (down && repeat != 0) return true;
        keys = down ? keys | mask : keys & ~mask;
        update(now);
        return true;
    }
    /** Sticks have already passed the device's flat calibration; trigger/hat values are raw. */
    void motion(float x, float y, float z, float rz, float lt, float rt, float hx, float hy, long now) {
        if (blockLeft && x == 0 && y == 0) blockLeft = false;
        if (blockRight && z == 0 && rz == 0) blockRight = false;
        if (blockTriggers && lt < .25f && rt < .25f) blockTriggers = false;
        if (blockHat && Math.abs(hx) < .5f && Math.abs(hy) < .5f) blockHat = false;
        lx = blockLeft ? 0 : x; ly = blockLeft ? 0 : y;
        rx = blockRight ? 0 : z; ry = blockRight ? 0 : rz;
        long next = 0;
        if (!blockTriggers) {
            if (lt > (has(axes, KeyEvent.KEYCODE_BUTTON_L2) ? .4f : .55f)) next |= bit(KeyEvent.KEYCODE_BUTTON_L2);
            if (rt > (has(axes, KeyEvent.KEYCODE_BUTTON_R2) ? .4f : .55f)) next |= bit(KeyEvent.KEYCODE_BUTTON_R2);
        }
        if (!blockHat) {
            if (hx < -.5f) next |= bit(KeyEvent.KEYCODE_DPAD_LEFT);
            if (hx > .5f) next |= bit(KeyEvent.KEYCODE_DPAD_RIGHT);
            if (hy < -.5f) next |= bit(KeyEvent.KEYCODE_DPAD_UP);
            if (hy > .5f) next |= bit(KeyEvent.KEYCODE_DPAD_DOWN);
        }
        axes = next;
        update(now);
    }
    private static int scan(int key) {
        switch (key) {
            case KeyEvent.KEYCODE_BUTTON_C: return 0x1C;
            case KeyEvent.KEYCODE_BUTTON_Z: return 0x0E;
            case KeyEvent.KEYCODE_BUTTON_X: return 0x39;
            case KeyEvent.KEYCODE_BUTTON_Y: return 0x19;
            case KeyEvent.KEYCODE_BUTTON_R2: return 0x2A;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 0x3B;
            case KeyEvent.KEYCODE_BUTTON_START: return 0x01;
            case KeyEvent.KEYCODE_DPAD_UP: return 0x4E;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0x4A;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0x2D;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x2E;
            default: return 0;
        }
    }
    private void apply(long next) {
        long changed = normal ^ next;
        normal = next;
        for (int key : KEYS) if ((changed & bit(key)) != 0) {
            boolean down = has(next, key);
            if (key == KeyEvent.KEYCODE_BUTTON_A || key == KeyEvent.KEYCODE_BUTTON_B)
                out.button(key == KeyEvent.KEYCODE_BUTTON_A ? 0 : 1, down);
            else if (scan(key) != 0) out.key(scan(key), down);
        }
    }
    private void update(long now) {
        long next = keys | axes, added = next & ~state;
        boolean selecting = has(next, KeyEvent.KEYCODE_BUTTON_SELECT);
        boolean wasSelecting = has(state, KeyEvent.KEYCODE_BUTTON_SELECT);
        if (next != state) activeAt = now;
        state = next;
        suppressed &= next;
        if (selecting) {
            if (!wasSelecting) selectUsed = false;
            suppressed |= next & ~bit(KeyEvent.KEYCODE_BUTTON_SELECT);
        }
        long before = normal;
        apply(selecting ? 0 : next & ~suppressed);
        if (selecting) {
            int[] slots = {KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_DOWN,
                    KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
                    KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1};
            for (int i = 0; i < slots.length; i++) if (has(added, slots[i])) {
                selectUsed = true;
                out.command('1' + i, (has(next, KeyEvent.KEYCODE_BUTTON_L2) ? 2 : 0) | (has(next, KeyEvent.KEYCODE_BUTTON_R2) ? 1 : 0));
            }
            if (has(added, KeyEvent.KEYCODE_BUTTON_R1)) { selectUsed = true; out.command('R', 2); }
            if (has(added, KeyEvent.KEYCODE_BUTTON_THUMBL)) { selectUsed = true; out.action(TOOLS); }
            if (has(added, KeyEvent.KEYCODE_BUTTON_THUMBR)) { selectUsed = true; out.action(KEYBOARD); }
            if (has(added, KeyEvent.KEYCODE_BUTTON_START)) { selectUsed = true; out.action(SETTINGS); }
        } else {
            long down = normal & ~before;
            if (has(down, KeyEvent.KEYCODE_BUTTON_THUMBL)) out.action(RESET_ZOOM);
            if (has(down, KeyEvent.KEYCODE_BUTTON_L1)) out.zoom(-1);
            if (has(down, KeyEvent.KEYCODE_BUTTON_R1)) out.zoom(1);
            if (has(down, KeyEvent.KEYCODE_BUTTON_L1) || has(down, KeyEvent.KEYCODE_BUTTON_R1)) repeatAt = now + 350;
            if (wasSelecting && !selectUsed) out.action(HELP);
        }
        if (!running()) frameAt = -1;
    }
    boolean running() { return lx != 0 || ly != 0 || rx != 0 || ry != 0 || has(normal, KeyEvent.KEYCODE_BUTTON_L1) || has(normal, KeyEvent.KEYCODE_BUTTON_R1); }
    boolean cursorVisible(long now) { return activeAt >= 0 && now - activeAt < CURSOR_IDLE_MS; }
    /** Frame time, not event frequency, controls speed; long stalls never jump the cursor. */
    void tick(long now) {
        float dt = Math.max(0, Math.min(50, frameAt < 0 ? 16 : now - frameAt)) / 1000f;
        frameAt = now;
        if (lx != 0 || ly != 0 || rx != 0 || ry != 0) {
            activeAt = now;
            if (has(state, KeyEvent.KEYCODE_BUTTON_SELECT)) selectUsed = true;
        }
        float length = (float)Math.hypot(lx, ly), magnitude = Math.min(1, length);
        float speed = length == 0 ? 0 : .95f * magnitude * (.2f + .8f * magnitude) / length;
        if (has(state, KeyEvent.KEYCODE_BUTTON_L2)) speed *= .22f;
        if (length > 0) out.move(lx * speed * dt, ly * speed * dt);
        float panLength = Math.max(1, (float)Math.hypot(rx, ry));
        if (rx != 0 || ry != 0) out.pan(rx / panLength * .8f * dt, ry / panLength * .8f * dt);
        if (now >= repeatAt) {
            int direction = (has(normal, KeyEvent.KEYCODE_BUTTON_R1) ? 1 : 0) - (has(normal, KeyEvent.KEYCODE_BUTTON_L1) ? 1 : 0);
            if (direction != 0) { out.zoom(direction); activeAt = now; }
            repeatAt = now + 120;
        }
    }
    /** Focus/touch takeover requires neutral sticks before motion can resume. */
    void cancel() {
        blockLeft |= lx != 0 || ly != 0; blockRight |= rx != 0 || ry != 0;
        blockTriggers |= has(axes, KeyEvent.KEYCODE_BUTTON_L2) || has(axes, KeyEvent.KEYCODE_BUTTON_R2);
        blockHat |= has(axes, KeyEvent.KEYCODE_DPAD_UP) || has(axes, KeyEvent.KEYCODE_DPAD_DOWN) ||
                has(axes, KeyEvent.KEYCODE_DPAD_LEFT) || has(axes, KeyEvent.KEYCODE_DPAD_RIGHT);
        keys = axes = state = suppressed = 0;
        lx = ly = rx = ry = 0;
        frameAt = activeAt = -1; selectUsed = true;
        apply(0);
    }
    void reset() {
        cancel();
        blockLeft = blockRight = blockTriggers = blockHat = false;
    }
}
