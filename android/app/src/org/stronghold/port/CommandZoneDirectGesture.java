package org.stronghold.port;

/** Direct zones distinguish a released tap from one consumed hold. */
final class CommandZoneDirectGesture {
    static final int NONE = 0, TAP = 1, HOLD = 2;
    private final float slop;
    private int pointer = -1;
    private float startX, startY;
    private long began;
    private boolean held;

    CommandZoneDirectGesture(float slop) {
        if (!Float.isFinite(slop) || slop < 0) throw new IllegalArgumentException("Invalid touch slop");
        this.slop = slop;
    }

    void begin(int pointer, float x, float y, long time) {
        cancel();
        if (pointer < 0 || !Float.isFinite(x) || !Float.isFinite(y) || time < 0) return;
        this.pointer = pointer; startX = x; startY = y; began = time;
    }

    boolean active() { return pointer >= 0; }

    void move(int pointer, float x, float y) {
        if (!active() || pointer != this.pointer) return;
        if (!Float.isFinite(x) || !Float.isFinite(y) || Math.hypot((double)x - startX, (double)y - startY) > slop)
            cancel();
    }

    boolean hold(long time) {
        if (!active() || held || time < began || time - began < CommandZoneGesture.HOLD_MS) return false;
        held = true; return true;
    }

    int end(int pointer, float x, float y, long time) {
        if (!active() || pointer != this.pointer) return NONE;
        move(pointer, x, y);
        int action = !active() || held || time < began ? NONE :
                time - began >= CommandZoneGesture.HOLD_MS ? HOLD : TAP;
        cancel(); return action;
    }

    void cancel() { pointer = -1; held = false; began = 0; }
}
