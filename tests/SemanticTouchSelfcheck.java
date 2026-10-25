package org.stronghold.port;

import java.util.ArrayList;

public final class SemanticTouchSelfcheck {
    static final class Event {
        final int action, sequence; final float x, y, dx, dy, scale;
        Event(int a, int s, float x, float y, float dx, float dy, float scale) {
            action = a; sequence = s; this.x = x; this.y = y; this.dx = dx; this.dy = dy; this.scale = scale;
        }
    }
    static final ArrayList<Event> events = new ArrayList<>();
    static SemanticTouchControls c;
    static void reset() { events.clear(); c = new SemanticTouchControls((a,s,x,y,dx,dy,z) -> events.add(new Event(a,s,x,y,dx,dy,z)), 8); c.bounds(0,0,1000,600); }
    static void check(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
    static int count(int action) { int n = 0; for (Event e : events) if (e.action == action) n++; return n; }
    static Event last() { return events.get(events.size() - 1); }
    static Event lastOf(int action) { Event found = null; for (Event e : events) if (e.action == action) found = e; return found; }
    static void down(int kind) { c.down(100,100,0); c.classified(c.sequence(), kind); }
    public static void main(String[] args) {
        reset(); down(SemanticTouchControls.MENU); c.move(180,130,100); c.up(210,140,200);
        check(count(SemanticTouchControls.UI_CLICK)==1&&last().x==210&&last().y==140&&count(SemanticTouchControls.PAN)==0,"menu activates only the final item on release");
        reset(); down(SemanticTouchControls.MENU); c.move(180,130,100); c.abort();
        check(count(SemanticTouchControls.UI_CLICK)==0,"cancelled menu swipe never activates");
        reset(); down(SemanticTouchControls.RADAR); c.move(130,115,100); c.move(145,120,140); c.up(145,120,180);
        float radarDx=0,radarDy=0;for(Event e:events)if(e.action==SemanticTouchControls.PAN){radarDx+=e.dx;radarDy+=e.dy;}
        check(radarDx==45&&radarDy==20&&count(SemanticTouchControls.UI_CLICK)==0&&last().action==SemanticTouchControls.UI_HOLD_END,"radar pans by relative deltas and releases without jumping");
        reset(); c.down(100,100,0); int token = c.sequence(); c.up(101,101,100);
        check(events.size() == 1 && last().action == SemanticTouchControls.QUERY, "no output before host classification");
        c.classified(token - 1, SemanticTouchControls.UI); check(events.size() == 1, "stale response ignored");
        c.classified(token, SemanticTouchControls.WORLD); check(last().action == SemanticTouchControls.TAP, "queued UP becomes semantic map tap");
        check(!c.active() && c.phase() == SemanticTouchControls.Phase.IDLE && c.classificationName().equals("WORLD") && c.fingerX() == 101, "diagnostics retain last classified point without guest access");
        c.down(100,100,180); c.classified(c.sequence(),SemanticTouchControls.WORLD); c.up(100,100,240);
        check(count(SemanticTouchControls.DOUBLE_TAP) == 1, "double tap is native same-type request");

        reset(); c.down(100,100,0); int first = c.sequence(); c.up(100,100,50);
        c.down(100,100,100); c.up(100,100,150); c.down(100,100,200); c.up(100,100,250);
        check(events.size() == 1 && c.sequence() == first, "finished pending taps wait in order instead of replacing first query");
        c.classified(first,SemanticTouchControls.WORLD); int second = c.sequence();
        check(count(SemanticTouchControls.TAP) == 1 && count(SemanticTouchControls.QUERY) == 2 && count(SemanticTouchControls.ABORT) == 0, "first UP drains before second query");
        c.classified(second,SemanticTouchControls.WORLD); int third = c.sequence();
        c.classified(third,SemanticTouchControls.WORLD);
        check(count(SemanticTouchControls.TAP) == 2 && count(SemanticTouchControls.DOUBLE_TAP) == 1 && count(SemanticTouchControls.QUERY) == 3, "three finished taps survive delayed classification");
        reset(); c.down(100,100,0); first = c.sequence(); c.up(100,100,50);
        c.down(100,100,100); c.move(140,100,200); c.move(200,100,700); c.up(200,100,800);
        c.classified(first,SemanticTouchControls.UI); c.classified(c.sequence(),SemanticTouchControls.WORLD);
        check(count(SemanticTouchControls.UI_CLICK) == 1 && count(SemanticTouchControls.PAN) > 0 && count(SemanticTouchControls.CANCEL) == 0, "deferred movement keeps original threshold time");
        reset(); c.down(100,100,0); first = c.sequence(); c.secondDown(100,100,200,100,20); c.pointerUp(100);
        c.down(300,100,150); c.up(300,100,200); c.classified(first,SemanticTouchControls.WORLD); c.classified(c.sequence(),SemanticTouchControls.UI);
        check(count(SemanticTouchControls.CANCEL) == 1 && count(SemanticTouchControls.UI_CLICK) == 1, "completed pending pair drains before next single finger gesture");
        reset(); c.down(100,100,0); first = c.sequence(); c.up(100,100,50); c.down(100,100,100); c.up(100,100,150); c.abort(); c.classified(first,SemanticTouchControls.UI);
        check(count(SemanticTouchControls.UI_CLICK) == 0 && count(SemanticTouchControls.ABORT) == 1, "OS abort drops deferred inputs without activating UI");
        reset(); c.down(100,100,0); first = c.sequence(); c.up(100,100,50); c.down(100,100,100);
        for(int i=0;i<130;i++) c.move(100+i,100,150+i);
        c.up(300,100,500); c.classified(first,SemanticTouchControls.UI);
        check(count(SemanticTouchControls.UI_CLICK) == 0 && count(SemanticTouchControls.ABORT) == 1, "bounded input overflow fails closed without delayed clicks");

        reset(); down(SemanticTouchControls.UI); c.up(100,100,100);
        check(count(SemanticTouchControls.UI_CLICK) == 1 && count(SemanticTouchControls.TAP) == 0, "UI cannot also order map");
        reset(); down(SemanticTouchControls.UI); c.hold(599); check(events.size() == 1, "tooltip waits");
        c.hold(600); c.hold(900); c.up(100,100,1000);
        check(count(SemanticTouchControls.UI_HOLD_BEGIN) == 1 && count(SemanticTouchControls.UI_HOLD_END) == 1 && count(SemanticTouchControls.UI_CLICK) == 0, "held UI only tooltip, never activation");
        reset(); c.down(100,100,0); c.up(100,100,700); c.classified(c.sequence(),SemanticTouchControls.UI);
        check(count(SemanticTouchControls.UI_CLICK) == 0 && count(SemanticTouchControls.UI_HOLD_END) == 1, "late reply preserves held UI intent");
        reset(); down(SemanticTouchControls.UI); c.move(140,100,100); c.up(140,100,200);
        check(count(SemanticTouchControls.UI_CLICK) == 0 && count(SemanticTouchControls.PAN) == 0 && count(SemanticTouchControls.UI_HOLD_END) == 1, "button drag retires hover without activating or panning map");
        check(count(SemanticTouchControls.UI_HOVER)>0,"UI movement remains ordinary hover until release");
        reset(); down(SemanticTouchControls.UI); c.move(140,100,100);c.move(180,120,150);c.move(210,140,200);
        check(last().action==SemanticTouchControls.UI_HOVER&&last().x==210&&last().y==140,"hover follows across menu items after slop without activation");
        c.up(210,140,250);check(last().action==SemanticTouchControls.UI_HOLD_END&&count(SemanticTouchControls.UI_CLICK)==0,"moving menu finger retires hover without a delayed activation");
        reset();down(SemanticTouchControls.UI);c.secondDown(100,100,200,100,20);c.pointerUp(100);
        check(count(SemanticTouchControls.CANCEL)==1&&count(SemanticTouchControls.UI_CLICK)==0&&count(SemanticTouchControls.TRANSFORM)==0,"deliberate two-finger UI tap asks engine to cancel only its armed command");
        reset();down(SemanticTouchControls.LIST);c.secondDown(100,100,200,100,20);c.pairMove(100,140,250,140);c.pointerUp(200);
        check(count(SemanticTouchControls.CANCEL)==0&&count(SemanticTouchControls.TRANSFORM)==0&&count(SemanticTouchControls.UI_CLICK)==0,"moving two fingers over UI cannot cancel or transform world");
        reset(); down(SemanticTouchControls.LIST); c.move(100,140,100); c.up(100,160,200);
        check(count(SemanticTouchControls.UI_SCROLL) >= 1 && count(SemanticTouchControls.UI_CLICK) == 0 && count(SemanticTouchControls.PAN) == 0 && last().action == SemanticTouchControls.UI_HOLD_END, "list drag releases its UI ownership without selecting or moving map");

        reset(); down(SemanticTouchControls.WORLD); c.move(130,115,100); c.move(145,120,140); c.up(145,120,180);
        check(count(SemanticTouchControls.PAN) >= 2 && count(SemanticTouchControls.BOX_BEGIN) == 0 && count(SemanticTouchControls.TAP) == 0, "ordinary drag pans directly");
        float dx = 0, dy = 0; for (Event e : events) if (e.action == SemanticTouchControls.PAN) { dx += e.dx; dy += e.dy; }
        check(dx == 45 && dy == 20, "smooth pan preserves exact total delta");
        reset(); c.down(100,100,0); c.move(130,100,100); c.classified(c.sequence(),SemanticTouchControls.WORLD); c.up(130,100,700);
        check(count(SemanticTouchControls.PAN) > 0 && count(SemanticTouchControls.BOX_BEGIN) == 0, "late classification uses original movement time");
        reset(); down(SemanticTouchControls.WORLD); c.move(140,150,350); c.up(150,160,500);
        check(count(SemanticTouchControls.BOX_BEGIN) == 1 && count(SemanticTouchControls.BOX_END) == 1 && count(SemanticTouchControls.PAN) == 0, "hold then drag selects rectangle");
        reset(); down(SemanticTouchControls.WORLD); c.hold(600); c.up(100,100,800);
        check(count(SemanticTouchControls.HOLD) == 1 && count(SemanticTouchControls.CANCEL) == 0 && count(SemanticTouchControls.TAP) == 0, "long press is one engine-resolved hold");
        reset(); c.down(100,100,0); c.move(140,100,650); c.classified(c.sequence(),SemanticTouchControls.WORLD); c.up(140,100,700);
        check(count(SemanticTouchControls.HOLD) == 1 && count(SemanticTouchControls.BOX_END) == 0, "late callback cannot turn expired long press into selection");
        reset(); c.down(100,100,0); c.up(100,100,700); c.classified(c.sequence(),SemanticTouchControls.WORLD);
        check(count(SemanticTouchControls.HOLD) == 1 && count(SemanticTouchControls.TAP) == 0, "late classification keeps a released long press");

        reset(); down(SemanticTouchControls.WORLD); c.secondDown(100,100,200,100,20); c.pairMove(120,110,240,110); c.pointerUp(200); c.up(240,110,210);
        check(count(SemanticTouchControls.TRANSFORM) == 1 && count(SemanticTouchControls.PAN) == 0 && count(SemanticTouchControls.CANCEL) == 0 && count(SemanticTouchControls.TAP) == 0, "two finger transform is atomic and surviving pointer cannot tap");
        Event transform = last(); check(transform.dx == 30 && transform.dy == 10 && Math.abs(transform.scale - 1.2f) < .0001f, "centroid translation and pinch factor share event");
        check(c.fingerX() == 180 && c.fingerY() == 110, "diagnostic point is pair centroid");
        reset(); c.down(100,100,0); c.secondDown(100,100,200,100,20); c.pointerUp(100); c.up(200,100,120); c.classified(c.sequence(),SemanticTouchControls.WORLD);
        check(count(SemanticTouchControls.CANCEL) == 1 && count(SemanticTouchControls.TAP) == 0, "pending two finger tap cancels once");
        reset(); down(SemanticTouchControls.WORLD); c.move(130,100,100); c.secondDown(130,100,200,100,150); c.pointerUp(200);
        check(count(SemanticTouchControls.CANCEL) == 0, "adding second finger after pan cannot become cancel tap");

        for (int kind : new int[]{SemanticTouchControls.ABILITY,SemanticTouchControls.BUILDING}) {
            reset(); down(kind); check(count(SemanticTouchControls.AIM_BEGIN) == 1 && count(SemanticTouchControls.AIM_COMMIT) == 0, "explicit aim point precedes preview");
            c.move(140,160,200); c.up(150,170,900);
            check(count(SemanticTouchControls.AIM_UPDATE) > 0 && count(SemanticTouchControls.AIM_COMMIT) == 1, "release commits aim at latest point");
            reset(); down(kind); c.hold(600); c.up(100,100,900);
            check(count(SemanticTouchControls.CANCEL) == 1 && count(SemanticTouchControls.AIM_COMMIT) == 0, "stationary armed hold cancels without firing or placing");
            reset(); down(kind); c.up(100,100,700);
            check(count(SemanticTouchControls.CANCEL) == 1 && count(SemanticTouchControls.AIM_COMMIT) == 0, "late hold timer cannot turn stationary long press into commit");
            reset(); c.down(100,100,0); c.up(100,100,700); c.classified(c.sequence(),kind);
            check(count(SemanticTouchControls.CANCEL) == 1 && count(SemanticTouchControls.AIM_COMMIT) == 0, "late classification preserves armed long-press cancellation");
            reset(); down(kind); c.secondDown(100,100,200,100,100); c.pointerUp(200); c.up(200,100,210);
            check(count(SemanticTouchControls.CANCEL) == 1 && count(SemanticTouchControls.AIM_COMMIT) == 0 && count(SemanticTouchControls.TRANSFORM) == 0, "second finger cancels aiming without pan or commit");
            reset(); down(kind); c.abort(); c.up(100,100,200);
            check(count(SemanticTouchControls.ABORT) == 1 && count(SemanticTouchControls.CANCEL) == 0 && count(SemanticTouchControls.AIM_COMMIT) == 0, "OS cancellation commits nothing and preserves command/selection");
        }
        reset(); c.down(100,100,0); int old = c.sequence(); c.abort(); c.classified(old,SemanticTouchControls.WORLD); c.up(100,100,100);
        check(count(SemanticTouchControls.TAP) == 0, "reply after OS cancellation ignored");
        reset(); c.down(-1,100,0); c.up(-1,100,100); check(events.isEmpty(), "letterbox touch ignored");
        reset();down(SemanticTouchControls.BOOK);c.hold(600);c.up(100,100,700);
        check(count(SemanticTouchControls.OPEN_TOOLS)==1&&count(SemanticTouchControls.UI_CLICK)==0,"book hold opens tools without opening original report");
        reset();down(SemanticTouchControls.BOOK);c.up(100,100,100);
        check(count(SemanticTouchControls.UI_CLICK)==1,"book short tap keeps original action");
        reset();down(SemanticTouchControls.RADAR);c.secondDown(100,100,130,100,20);c.pointerUp(100);c.up(130,100,110);
        check(count(SemanticTouchControls.RADAR_TOGGLE)==1&&count(SemanticTouchControls.CANCEL)==0&&count(SemanticTouchControls.UI_CLICK)==0,"simultaneous radar tap does not cancel units");
        reset();c.down(100,100,0);c.secondDown(100,100,130,100,20);c.pointerUp(100);c.up(100,100,110);c.classified(c.sequence(),SemanticTouchControls.RADAR);
        check(count(SemanticTouchControls.RADAR_TOGGLE)==1&&count(SemanticTouchControls.UI_CLICK)==0,"completed radar pair survives delayed engine classification");
        reset();down(SemanticTouchControls.RADAR);c.hold(600);c.up(100,100,700);
        check(count(SemanticTouchControls.HOLD)==1&&count(SemanticTouchControls.UI_HOLD_BEGIN)==0&&count(SemanticTouchControls.UI_CLICK)==0,"radar hold toggles relief instead of an empty description");
        reset();down(SemanticTouchControls.WORLD);c.secondDown(100,100,200,100,20);
        for(int deg=5;deg<=60;deg+=5){double a=Math.toRadians(deg);c.pairMove(150-50*(float)Math.cos(a),100-50*(float)Math.sin(a),150+50*(float)Math.cos(a),100+50*(float)Math.sin(a));}
        check(count(SemanticTouchControls.ROTATE)==1&&lastOf(SemanticTouchControls.ROTATE).dx==1,"clockwise twist past the detent turns once");
        for(int deg=55;deg>=30;deg-=5){double a=Math.toRadians(deg);c.pairMove(150-50*(float)Math.cos(a),100-50*(float)Math.sin(a),150+50*(float)Math.cos(a),100+50*(float)Math.sin(a));}
        check(count(SemanticTouchControls.ROTATE)==2&&lastOf(SemanticTouchControls.ROTATE).dx==-1,"twisting back past the hysteresis turns back");
        c.pointerUp(200);c.up(150,100,210);
        check(count(SemanticTouchControls.CANCEL)==0&&count(SemanticTouchControls.TAP)==0,"a twist is never a two-finger cancel tap");
        reset();down(SemanticTouchControls.WORLD);c.secondDown(100,100,200,100,20);
        for(int deg=-5;deg>=-150;deg-=5){double a=Math.toRadians(deg);c.pairMove(150-50*(float)Math.cos(a),100-50*(float)Math.sin(a),150+50*(float)Math.cos(a),100+50*(float)Math.sin(a));}
        check(count(SemanticTouchControls.ROTATE)==2&&lastOf(SemanticTouchControls.ROTATE).dx==-1,"counter-clockwise half turn makes two quarter turns");
        reset();down(SemanticTouchControls.UI);c.secondDown(100,100,200,100,20);
        for(int deg=5;deg<=90;deg+=5){double a=Math.toRadians(deg);c.pairMove(150-50*(float)Math.cos(a),100-50*(float)Math.sin(a),150+50*(float)Math.cos(a),100+50*(float)Math.sin(a));}
        check(count(SemanticTouchControls.ROTATE)==0,"a twist over the interface never turns the map");
        reset();c.buildingSwipes(true);down(SemanticTouchControls.BUILD_PANEL);c.move(160,100,120);c.up(180,100,200);
        check(count(SemanticTouchControls.BUILD_SWIPE)==1&&count(SemanticTouchControls.UI_CLICK)==0&&count(SemanticTouchControls.PAN)==0,"building swipe consumes the whole gesture");
        reset();c.buildingSwipes(false);down(SemanticTouchControls.BUILD_PANEL);c.move(180,100,120);c.up(180,100,200);
        check(count(SemanticTouchControls.BUILD_SWIPE)==0&&count(SemanticTouchControls.UI_CLICK)==0,"building swipes can be disabled");
        /* Placement loupe: never on a quick tap; after 120 ms of rest or at once when the aim moves. */
        for (int kind : new int[]{SemanticTouchControls.ABILITY,SemanticTouchControls.BUILDING}) {
            reset(); down(kind);
            check(c.loupe(50)==70&&c.loupe(119)==1&&c.loupe(120)==0&&c.loupe(400)==0,"a resting aim shows the loupe after 120 ms");
            c.up(100,100,90); check(c.loupe(200)==-1&&count(SemanticTouchControls.AIM_COMMIT)==1,"a quick tap places without it");
            reset(); down(kind); c.move(104,103,20); check(c.loupe(20)==100,"jitter within the slop is still a rest");
            c.move(140,100,40); check(c.loupe(40)==0,"a moving aim shows it at once");
            c.secondDown(140,100,200,100,60); check(c.loupe(60)==-1,"a second finger hides it");
            reset(); down(kind); c.hold(600); check(c.loupe(600)==-1&&count(SemanticTouchControls.CANCEL)==1,"the cancelling hold hides it");
            reset(); down(kind); c.abort(); check(c.loupe(10)==-1,"an OS abort hides it");
        }
        reset(); down(SemanticTouchControls.WORLD); check(c.loupe(500)==-1,"no loupe without an armed placement");
        reset(); c.down(100,100,0); check(c.loupe(200)==-1,"nor before the engine has classified the touch");
        c.classified(c.sequence(),SemanticTouchControls.BUILDING); check(c.loupe(200)==0,"a late classification shows it if the finger rested long enough");
        reset();down(SemanticTouchControls.DRAG);c.move(220,160,100);c.up(240,180,200);
        check(count(SemanticTouchControls.UI_DRAG_BEGIN)==1&&count(SemanticTouchControls.UI_DRAG_UPDATE)>0&&
                count(SemanticTouchControls.UI_DRAG_END)==1&&count(SemanticTouchControls.UI_CLICK)==0&&
                count(SemanticTouchControls.PAN)==0&&last().x==240,"spawn icon drag retains original down/move/up without a click or pan");
        reset();c.down(100,100,0);c.move(220,160,100);c.up(240,180,200);c.classified(c.sequence(),SemanticTouchControls.DRAG);
        check(count(SemanticTouchControls.UI_DRAG_BEGIN)==1&&count(SemanticTouchControls.UI_DRAG_END)==1&&
                last().action==SemanticTouchControls.UI_DRAG_END&&last().x==240,"delayed classification preserves both spawn drag endpoints");
        reset();down(SemanticTouchControls.DRAG);c.move(220,160,100);c.abort();c.up(240,180,200);
        check(count(SemanticTouchControls.ABORT)==1&&count(SemanticTouchControls.UI_DRAG_END)==0,"OS cancellation never drops a spawn icon");
        reset();down(SemanticTouchControls.DRAG);c.secondDown(100,100,200,100,100);c.pointerUp(200);
        check(count(SemanticTouchControls.ABORT)==1&&count(SemanticTouchControls.UI_DRAG_END)==0,"second finger aborts a spawn drag without a drop");
        System.out.println("SemanticTouch: queued classification, UI, tooltip, scroll, world, hold, aim, transform, twist turns, book, radar, optional swipes, loupe timing and OS cancellation PASS");
    }
}
