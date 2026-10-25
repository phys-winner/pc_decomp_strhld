package org.stronghold.port;

/** Let the original polling UI see hover before a touch-generated press. */
final class MouseGate {
    interface Output {
        void button(int button, boolean down);
        void move(float x, float y, boolean warp);
    }
    interface Scheduler { void post(Runnable work, long delay); void remove(Runnable work); }
    private final Output out;
    private final Scheduler scheduler;
    private final boolean[] down = new boolean[2];
    private Runnable press, release;
    private boolean pendingDrag, pendingMove;
    private float nextX, nextY;

    MouseGate(Output output, Scheduler timer) { out = output; scheduler = timer; }
    private void button(int button, boolean value) {
        if (down[button] != value) { down[button] = value; out.button(button, value); }
    }
    void cancel() {
        if (press != null) scheduler.remove(press);
        if (release != null) scheduler.remove(release);
        press = release = null; pendingDrag = pendingMove = false;
        button(0, false); button(1, false);
    }
    void hold(int button, boolean value) { cancel(); button(button, value); }
    void click(final int button) {
        cancel();
        press = () -> {
            press = null; button(button, true);
            release = () -> { release = null; button(button, false); };
            scheduler.post(release, 90);
        };
        scheduler.post(press, 30);
    }
    void beginDrag() {
        cancel(); pendingDrag = true;
        press = () -> {
            press = null; pendingDrag = false; button(0, true);
            if (pendingMove) { out.move(nextX, nextY, false); pendingMove = false; }
        };
        scheduler.post(press, 30);
    }
    void move(float x, float y, boolean warp) {
        if (pendingDrag && !warp) { nextX = x; nextY = y; pendingMove = true; }
        else out.move(x, y, warp);
    }
}
