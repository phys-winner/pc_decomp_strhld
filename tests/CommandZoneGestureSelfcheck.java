package org.stronghold.port;

public final class CommandZoneGestureSelfcheck {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static void idle(CommandZoneGesture gesture) {
        check(!gesture.active() && gesture.selected() == -1 && !gesture.held(Long.MAX_VALUE), "Release/cancel leaves no gesture");
    }
    public static void main(String[] args) {
        CommandZoneGesture gesture = new CommandZoneGesture();
        idle(gesture);
        check(CommandZoneGesture.HOLD_MS == 300, "Hold threshold stays 300 ms");
        gesture.begin(4, 1000);
        check(gesture.active() && !gesture.held(1299) && !gesture.held(999), "No early or backwards-time hold");
        check(!gesture.move(4, 1299, 2) && gesture.selected() == -1, "Hover before hold cannot select");
        check(gesture.end(4, 1299, 2) == -1, "Quick tap cannot execute a command");
        idle(gesture);

        gesture.begin(4, 2000);
        check(gesture.held(2300), "Hold activates exactly at threshold");
        check(gesture.move(4, 2300, 2) && gesture.selected() == 2, "Hold selects current command");
        check(!gesture.move(4, 2301, 2), "Same command does not change selection");
        check(gesture.move(4, 2302, 5) && gesture.selected() == 5, "Drag selects another command");
        check(gesture.move(4, 2303, -1) && gesture.selected() == -1, "Leaving commands clears selection");
        check(!gesture.move(4, 2304, -7), "Any negative hit means no command");
        check(gesture.move(4, 2305, 3), "Reenter command after leaving");
        check(gesture.end(4, 2306, 6) == 6, "Release uses the final command under the pointer");
        idle(gesture);
        check(gesture.end(4, 2400, 6) == -1 && !gesture.move(4, 2400, 6), "Repeated release or movement cannot execute stale command");

        gesture.begin(0, 3000);
        check(gesture.end(0, 3300, 0) == 0, "A held release may select without an earlier move");
        idle(gesture);
        gesture.begin(0, 4000);
        gesture.move(0, 4300, 1);
        check(gesture.end(0, 4301, -1) == -1, "Release outside cancels an earlier selection");
        idle(gesture);

        gesture.begin(7, 5000);
        check(!gesture.move(8, 5300, 1) && gesture.selected() == -1, "Another pointer cannot select");
        gesture.move(7, 5300, 2);
        check(!gesture.move(8, 5301, 4) && gesture.selected() == 2, "Another pointer cannot replace selection");
        check(gesture.end(8, 5302, 4) == -1, "Another pointer cannot execute");
        idle(gesture);
        check(gesture.end(7, 5303, 2) == -1, "Mismatched release cancels the owner too");

        gesture.begin(1, 6000);
        gesture.move(1, 6300, 3);
        gesture.cancel();
        idle(gesture);
        check(gesture.end(1, 6400, 3) == -1, "Cancellation prevents stale release");
        gesture.begin(1, 7000);
        gesture.move(1, 7300, 3);
        gesture.begin(2, 7400);
        check(gesture.selected() == -1 && !gesture.held(7699), "New gesture resets selection and timer");
        check(gesture.end(2, 7699, 3) == -1, "New quick tap cannot inherit old hold");
        gesture.begin(-1, 8000);
        idle(gesture);

        check(CommandZoneGesture.contains(true, 10, 20, 30, 40, 10, 20), "Top-left edge belongs to zone");
        check(CommandZoneGesture.contains(true, 10, 20, 30, 40, 39.5f, 59.5f), "Interior belongs to zone");
        check(!CommandZoneGesture.contains(true, 10, 20, 30, 40, 40, 30), "Right edge is exclusive");
        check(!CommandZoneGesture.contains(true, 10, 20, 30, 40, 20, 60), "Bottom edge is exclusive");
        check(!CommandZoneGesture.contains(true, 10, 20, 30, 40, 9, 20), "Outside is not captured");
        check(!CommandZoneGesture.contains(false, 10, 20, 30, 40, 20, 30), "Empty zone passes input through");
        for (float invalid : new float[]{0, -1, Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY}) {
            check(!CommandZoneGesture.contains(true, 10, 20, invalid, 40, 20, 30), "Invalid width passes through");
            check(!CommandZoneGesture.contains(true, 10, 20, 30, invalid, 20, 30), "Invalid height passes through");
        }
        for (float invalid : new float[]{Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY}) {
            check(!CommandZoneGesture.contains(true, invalid, 20, 30, 40, 20, 30), "Invalid left passes through");
            check(!CommandZoneGesture.contains(true, 10, invalid, 30, 40, 20, 30), "Invalid top passes through");
            check(!CommandZoneGesture.contains(true, 10, 20, 30, 40, invalid, 30), "Invalid pointer X passes through");
            check(!CommandZoneGesture.contains(true, 10, 20, 30, 40, 20, invalid), "Invalid pointer Y passes through");
        }
        System.out.println("Command zones: hold boundary, drag/release selection, pointer ownership, cancellation and empty/invalid pass-through: PASS");
    }
}
