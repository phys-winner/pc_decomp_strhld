package org.stronghold.port;

public final class PointerRedrawSelfcheck {
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) {
        PointerRedraw redraw = new PointerRedraw();
        for (int i = 0; i < 1000; i++) check(!redraw.needed(false), "Empty pan must not submit an Android frame");
        for (int i = 0; i < 1000; i++) check(redraw.needed(true), "Every visible cursor/overlay movement must redraw");
        check(redraw.needed(false), "Hide must erase the previous cursor/overlay");
        for (int i = 0; i < 1000; i++) check(!redraw.needed(false), "Hidden overlay stays idle after erase");
        check(redraw.needed(true), "Showing the overlay again must redraw");
        check(redraw.needed(false), "Repeated show/hide must erase once again");
        check(!redraw.needed(false), "No redundant erase");
        System.out.println("Pointer overlay: empty input frames suppressed; visible redraws and one final erase retained: PASS");
    }
}
