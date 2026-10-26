package org.stronghold.port;

/** A command zone owns one pointer until release; a quick tap never executes a command. */
final class CommandZoneGesture {
    static final long HOLD_MS = 300;
    private int pointer = -1, selection = -1;
    private long began;

    void begin(int pointer, long time) {
        cancel();
        if (pointer < 0) return;
        this.pointer = pointer;
        began = time;
    }

    boolean move(int pointer, long time, int hitIndex) {
        if (!active() || pointer != this.pointer) return false;
        int next = held(time) && hitIndex >= 0 ? hitIndex : -1;
        boolean changed = selection != next;
        selection = next;
        return changed;
    }

    int selected() { return selection; }
    boolean held(long time) { return active() && time >= began && time - began >= HOLD_MS; }
    boolean active() { return pointer >= 0; }

    int end(int pointer, long time, int hitIndex) {
        int chosen = active() && pointer == this.pointer && held(time) && hitIndex >= 0 ? hitIndex : -1;
        cancel();
        return chosen;
    }

    void cancel() { pointer = selection = -1; began = 0; }

    static boolean contains(boolean nonempty, float left, float top, float width, float height, float x, float y) {
        return nonempty && Float.isFinite(left) && Float.isFinite(top) && Float.isFinite(width) &&
                Float.isFinite(height) && Float.isFinite(x) && Float.isFinite(y) && width > 0 && height > 0 &&
                x >= left && y >= top && (double)x < (double)left + width && (double)y < (double)top + height;
    }
}
