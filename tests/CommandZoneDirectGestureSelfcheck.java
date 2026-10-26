package org.stronghold.port;

public final class CommandZoneDirectGestureSelfcheck {
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static void idle(CommandZoneDirectGesture gesture) {
        check(!gesture.active() && !gesture.hold(Long.MAX_VALUE), "No action without an active pointer");
        check(gesture.end(0, 10, 20, Long.MAX_VALUE) == CommandZoneDirectGesture.NONE, "No stale release");
    }
    public static void main(String[] args) {
        CommandZoneDirectGesture g = new CommandZoneDirectGesture(5);
        idle(g);
        g.begin(0, 10, 20, 1000);
        check(g.active() && !g.hold(999) && !g.hold(1299), "No early or backwards hold");
        check(g.end(0, 10, 20, 1299) == CommandZoneDirectGesture.TAP, "Release before 300 ms taps");
        idle(g);

        g.begin(2, 10, 20, 2000);
        check(g.hold(2300), "Hold fires at exactly 300 ms");
        check(!g.hold(2301) && !g.hold(4000), "Hold fires only once");
        check(g.end(2, 10, 20, 4000) == CommandZoneDirectGesture.NONE, "Consumed hold release never taps");
        idle(g);
        g.begin(0, 10, 20, 5000);
        check(g.end(0, 10, 20, 5300) == CommandZoneDirectGesture.HOLD, "Delayed timer release still holds");
        idle(g);
        g.begin(0, 10, 20, 6000);
        check(g.end(0, 10, 20, 8000) == CommandZoneDirectGesture.HOLD,
                "A long press stays a hold even when the caller has no configured hold command");
        check(g.end(0, 10, 20, 8001) == CommandZoneDirectGesture.NONE, "A repeated release cannot repeat hold");

        g.begin(3, 10, 20, 9000);
        g.move(3, 13, 24);
        check(g.active() && g.end(3, 13, 24, 9100) == CommandZoneDirectGesture.TAP, "Exactly slop is allowed");
        g.begin(3, 10, 20, 10000);
        g.move(3, 13.01f, 24);
        check(!g.active() && !g.hold(10300), "Diagonal movement beyond slop cancels hold");
        check(g.end(3, 10, 20, 10100) == CommandZoneDirectGesture.NONE, "Returning after a drag does not tap");
        g.begin(3, 10, 20, 11000);
        check(g.end(3, 16, 20, 11100) == CommandZoneDirectGesture.NONE, "Release displacement also cancels");

        g.begin(4, 10, 20, 12000);
        g.move(5, Float.NaN, 200);
        check(g.active(), "Another pointer cannot cancel the owner");
        check(g.end(5, 10, 20, 12100) == CommandZoneDirectGesture.NONE && g.active(), "Another pointer release is ignored");
        check(g.end(4, 10, 20, 12101) == CommandZoneDirectGesture.TAP, "Owner still releases after unrelated pointer");
        g.begin(4, 10, 20, 13000); g.cancel();
        check(g.end(4, 10, 20, 13050) == CommandZoneDirectGesture.NONE, "Second pointer or lifecycle cancel cannot tap");
        idle(g);

        for (int i = 0; i < 10; i++) {
            long time = 14000 + i * 50;
            g.begin(0, 10, 20, time);
            check(g.end(0, 10, 20, time + 20) == CommandZoneDirectGesture.TAP, "Each rapid tap executes once");
            check(!g.hold(time + 300), "Old hold timer cannot execute after a rapid tap");
        }
        g.begin(0, 10, 20, 15000); g.end(0, 10, 20, 15020);
        g.begin(1, 10, 20, 15200);
        check(!g.hold(15300) && g.hold(15500), "A stale timer cannot hold the new gesture early");
        g.cancel(); idle(g);
        g.begin(0, 10, 20, 16000);
        g.begin(1, 10, 20, 16100);
        check(!g.hold(16300) && g.end(1, 10, 20, 16301) == CommandZoneDirectGesture.TAP, "New down resets the old timer");
        g.begin(0, 10, 20, 17000);
        check(g.end(0, 10, 20, 16999) == CommandZoneDirectGesture.NONE, "Backwards release cannot tap");
        idle(g);

        for (float invalid : new float[]{Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY}) {
            g.begin(0, invalid, 20, 18000); idle(g);
            g.begin(0, 10, invalid, 18000); idle(g);
            g.begin(0, 10, 20, 18000); g.move(0, invalid, 20); idle(g);
            g.begin(0, 10, 20, 18000); g.move(0, 10, invalid); idle(g);
        }
        g.begin(-1, 10, 20, 18000); idle(g);
        g.begin(0, 10, 20, -1); idle(g);
        CommandZoneDirectGesture exact = new CommandZoneDirectGesture(0);
        exact.begin(0, 0, 0, 0); exact.move(0, 0, 0);
        check(exact.end(0, 0, 0, 0) == CommandZoneDirectGesture.TAP, "Zero slop allows a stationary tap");
        exact.begin(0, 0, 0, 0); exact.move(0, .001f, 0); idle(exact);
        for (float invalid : new float[]{-1, Float.NaN, Float.POSITIVE_INFINITY}) {
            try { new CommandZoneDirectGesture(invalid); throw new AssertionError("Invalid slop accepted"); }
            catch (IllegalArgumentException expected) {}
        }
        System.out.println("Direct command zones: tap/hold boundary, delayed timer, slop, cancellation, pointer ownership and repeat suppression PASS");
    }
}
