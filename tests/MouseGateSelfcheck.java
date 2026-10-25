package org.stronghold.port;

import java.util.ArrayList;

public final class MouseGateSelfcheck {
    static final ArrayList<String> events = new ArrayList<>();
    static Runnable task;
    static long delay;
    static final MouseGate gate = new MouseGate(new MouseGate.Output() {
        public void button(int b, boolean d) { events.add("button:" + b + ":" + d); }
        public void move(float x, float y, boolean warp) { events.add("move:" + (int)x + ":" + (int)y + ":" + warp); }
    }, new MouseGate.Scheduler() {
        public void post(Runnable r, long ms) { if (task != null) throw new AssertionError("two timers"); task = r; delay = ms; }
        public void remove(Runnable r) { if (task == r) task = null; }
    });
    static void check(boolean ok, String name) { if (!ok) throw new AssertionError(name + ": " + events); }
    static void fire(long ms) { check(task != null && delay == ms, "timer delay"); Runnable r = task; task = null; r.run(); }
    static void reset() { gate.cancel(); events.clear(); }
    public static void main(String[] args) {
        reset(); gate.move(100, 100, true); gate.click(0);
        check(events.size() == 1, "hover before press"); fire(30); fire(90);
        check(events.toString().equals("[move:100:100:true, button:0:true, button:0:false]"), "tap ordering");
        reset(); gate.click(0); gate.cancel(); check(task == null && events.isEmpty(), "pan cancels pending click");
        reset(); gate.click(0); fire(30); gate.cancel(); check(task == null && events.toString().equals("[button:0:true, button:0:false]"), "focus releases held click");
        reset(); gate.move(100, 100, true); gate.beginDrag(); gate.move(140, 120, false); gate.move(160, 150, false);
        check(events.size() == 1, "drag cannot move past hover before press"); fire(30); gate.cancel();
        check(events.toString().equals("[move:100:100:true, button:0:true, move:160:150:false, button:0:false]"), "drag ordering/latest motion");
        reset(); gate.beginDrag(); gate.move(10, 10, false); gate.cancel(); check(task == null && events.isEmpty(), "second finger cancels queued drag");
        reset(); gate.click(0); gate.click(1); fire(30); fire(90);
        check(events.toString().equals("[button:1:true, button:1:false]"), "RMB replaces stale LMB");
        reset(); gate.hold(0, true); check(task == null && events.toString().equals("[button:0:true]"), "manual hold immediate"); gate.hold(0, false);
        System.out.println("MouseGate hover delay, drag ordering, replacement and cancellation: PASS");
    }
}
