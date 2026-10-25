package org.stronghold.port;

import java.util.ArrayDeque;

/** Android-independent gestures. The engine, not screen rectangles, classifies a touch. */
final class SemanticTouchControls {
    // Keep in sync with android/host/touch_api.h; tests compare both definitions.
    static final int QUERY = 0, TAP = 1, DOUBLE_TAP = 2, BOX_BEGIN = 3, BOX_UPDATE = 4, BOX_END = 5,
            PAN = 6, TRANSFORM = 7, AIM_BEGIN = 8, AIM_UPDATE = 9, AIM_COMMIT = 10, CANCEL = 11,
            UI_CLICK = 12, UI_HOLD_BEGIN = 13, UI_HOLD_END = 14, UI_SCROLL = 15, ABORT = 16, UI_SCALE = 17, MODE = 18, UI_HOVER = 19,
            OPEN_TOOLS = 20, RADAR_TOGGLE = 21, BUILD_SWIPE = 22, HOLD = 23, ZOOM_RESET = 24, ROTATE = 25,
            WHEEL = 26, GRAB = 27, UI_DRAG_BEGIN = 28, UI_DRAG_UPDATE = 29, UI_DRAG_END = 30;
    static final int PENDING = 0, UI = 1, LIST = 2, WORLD = 3, ABILITY = 4, BUILDING = 5, BLOCKED = 6, BOOK = 7, RADAR = 8, BUILD_PANEL = 9, DRAG = 10, MENU = 11;
    static final long BOX_HOLD_MS = 300, LONG_HOLD_MS = 600, DOUBLE_TAP_MS = 300, LOUPE_MS = 120;
    /* A two-finger twist turns the original camera in quarter turns. A turn
     * fires past the half-way angle plus hysteresis, like a knob's detent. */
    static final double TURN = Math.PI / 2, TURN_HYSTERESIS = Math.toRadians(10), TWIST_SLOP = Math.toRadians(12);
    interface Output { void event(int action, int sequence, float x, float y, float dx, float dy, float scale); }
    enum Phase { IDLE, WAITING, READY, PANNING, BOX, AIMING, UI_HOLD, SCROLLING, DRAGGING, PAIR, DONE }
    private final Output out;
    private final float slop;
    private int sequence, kind = PENDING;
    private Phase phase = Phase.IDLE;
    private boolean active, ended, pair, pairEnded, moved, pairMoved;
    private long downAt, endedAt, firstMoveAt, pairAt, lastTapAt = -1;
    private float startX, startY, x, y, sentX, sentY;
    private float pairStartX, pairStartY, pairX, pairY, pairSpan, initialSpan, sentSpan;
    private double pairAngle, twist;
    private int turns;
    private float lastTapX, lastTapY, left, top, right = 1, bottom = 1;
    private static final int DEFERRED_LIMIT = 128;
    private final ArrayDeque<Runnable> deferred = new ArrayDeque<>();
    private boolean replaying;
    private boolean buildingSwipes;

    SemanticTouchControls(Output output, float touchSlop) { out = output; slop = touchSlop; }
    void bounds(float l, float t, float r, float b) { left = l; top = t; right = r; bottom = b; }
    int sequence() { return sequence; }
    boolean awaitingClassification() { return active && kind == PENDING; }
    Phase phase() { return phase; }
    void buildingSwipes(boolean enabled) { buildingSwipes=enabled; }
    private boolean ui() { return kind==UI||kind==LIST||kind==BOOK||kind==RADAR||kind==BUILD_PANEL||kind==MENU; }
    String classificationName() {
        String[] names = {"PENDING", "UI", "LIST", "WORLD", "ABILITY", "BUILDING", "BLOCKED", "BOOK", "RADAR", "BUILD_PANEL", "DRAG", "MENU"};
        return kind >= 0 && kind < names.length ? names[kind] : "BLOCKED";
    }
    boolean active() { return active; }
    /** The placement loupe follows an aiming finger once it has moved or
     * rested briefly, so a quick tap never flashes it: 0 shown, otherwise the
     * milliseconds until it would show, or -1 while there is nothing to aim. */
    long loupe(long time) {
        if (!active || ended || pair || phase != Phase.AIMING) return -1;
        return moved ? 0 : Math.max(0, downAt + LOUPE_MS - time);
    }
    float fingerX() { return pair ? pairX : x; }
    float fingerY() { return pair ? pairY : y; }
    private static float distance(float x, float y) { return (float)Math.hypot(x, y); }
    private void send(int action, float px, float py, float dx, float dy, float scale) {
        out.event(action, sequence, px, py, dx, dy, scale);
    }
    private void at(int action, float px, float py) { send(action, px, py, 0, 0, 1); }
    private boolean endedPending() { return active && (ended || pairEnded) && kind == PENDING; }
    private boolean defer(Runnable input) {
        if (replaying || (deferred.isEmpty() && !endedPending())) return false;
        if (deferred.size() == DEFERRED_LIMIT) { abort(); return true; }
        deferred.addLast(input); return true;
    }
    private void drainDeferred() {
        replaying = true;
        try {
            while (!deferred.isEmpty() && !endedPending()) deferred.removeFirst().run();
        } finally { replaying = false; }
    }
    void down(float px, float py, long time) {
        if (defer(() -> down(px, py, time))) return;
        abortCurrent(); ++sequence; if (sequence <= 0) sequence = 1;
        active = true; kind = PENDING; phase = Phase.WAITING;
        startX = x = sentX = px; startY = y = sentY = py; downAt = time; firstMoveAt = -1;
        if (px < left || px > right || py < top || py > bottom) { kind = BLOCKED; phase = Phase.DONE; return; }
        at(QUERY, px, py);
    }
    void classified(int id, int classification) {
        if (!active || id != sequence || kind != PENDING || classification == PENDING) return;
        kind = classification; phase = Phase.READY;
        if (kind == BLOCKED || kind < UI || kind > MENU) { phase = Phase.DONE; if (ended) active = false; drainDeferred(); return; }
        if (pair) {
            beginPair(); if (pairEnded) finishPair(); drainDeferred(); return;
        }
        if (kind == ABILITY || kind == BUILDING) { phase = Phase.AIMING; at(AIM_BEGIN, startX, startY); }
        else if (kind == DRAG) { phase = Phase.DRAGGING; at(UI_DRAG_BEGIN, startX, startY); }
        movement();
        if (ended) finish();
        drainDeferred();
    }
    void move(float px, float py, long time) {
        if (defer(() -> move(px, py, time))) return;
        if (!active || ended || pair) return;
        x = px; y = py;
        if (phase == Phase.DONE) { if(kind==UI||kind==MENU) at(UI_HOVER,x,y); return; }
        if (!moved && distance(x - startX, y - startY) > slop) { moved = true; firstMoveAt = time; }
        if (kind != PENDING) movement();
    }
    private void movement() {
        if (!active || phase == Phase.DONE) return;
        if (phase == Phase.DRAGGING) {
            if (x != sentX || y != sentY) at(UI_DRAG_UPDATE, x, y);
        } else if (phase == Phase.AIMING) {
            if (x != sentX || y != sentY) at(AIM_UPDATE, x, y);
        } else if (kind == RADAR && moved) {
            phase = Phase.PANNING;
            if (x != sentX || y != sentY) send(PAN, startX, startY, x - sentX, y - sentY, 1);
        } else if (kind == UI || kind==BOOK || kind==RADAR || kind==BUILD_PANEL || kind==MENU) {
            if (moved) { if(phase==Phase.UI_HOLD)at(UI_HOLD_END,x,y); phase = Phase.DONE; }
            if(x!=sentX||y!=sentY)at(UI_HOVER,x,y);
        } else if (kind == LIST) {
            if (moved) {
                if (phase == Phase.UI_HOLD) at(UI_HOLD_END, x, y);
                phase = Phase.SCROLLING;
                if (x != sentX || y != sentY) send(UI_SCROLL, startX, startY, x - sentX, y - sentY, 1);
            }
        } else if (kind == WORLD && moved) {
            if (phase == Phase.READY) {
                lastTapAt = -1;
                if (firstMoveAt - downAt >= LONG_HOLD_MS) { at(HOLD, startX, startY); phase = Phase.DONE; return; }
                if (firstMoveAt - downAt >= BOX_HOLD_MS) { phase = Phase.BOX; at(BOX_BEGIN, startX, startY); }
                else phase = Phase.PANNING;
            }
            if (phase == Phase.BOX) at(BOX_UPDATE, x, y);
            else if (phase == Phase.PANNING && (x != sentX || y != sentY)) send(PAN, x, y, x - sentX, y - sentY, 1);
        }
        sentX = x; sentY = y;
    }
    void hold(long time) {
        if (!active || ended || pair || moved || kind == PENDING || phase == Phase.DONE || time - downAt < LONG_HOLD_MS) return;
        if(kind==BOOK) { at(OPEN_TOOLS,startX,startY);phase=Phase.DONE; }
        /* The radar has no description; its hold toggles the relief view. */
        else if (kind == RADAR) { at(HOLD, startX, startY); phase = Phase.DONE; }
        else if (ui() && phase != Phase.UI_HOLD) { phase = Phase.UI_HOLD; at(UI_HOLD_BEGIN, startX, startY); }
        /* The engine decides: cancel a selection/command, or toggle relief. */
        else if (kind == WORLD) { at(HOLD, startX, startY); lastTapAt = -1; phase = Phase.DONE; }
        else if (kind == ABILITY || kind == BUILDING) { at(CANCEL, startX, startY); lastTapAt = -1; phase = Phase.DONE; }
    }
    void up(float px, float py, long time) {
        if (defer(() -> up(px, py, time))) return;
        if (!active || pair) return;
        move(px, py, time); ended = true; endedAt = time;
        if (kind != PENDING) finish();
    }
    private void finish() {
        if(kind==BUILD_PANEL&&buildingSwipes&&moved&&Math.abs(x-startX)>=slop*5&&Math.abs(x-startX)>Math.abs(y-startY)*1.5) {
            send(BUILD_SWIPE,startX,startY,x-startX,0,1);active=false;phase=Phase.IDLE;return;
        }
        if (kind == MENU && (moved || endedAt - downAt < LONG_HOLD_MS)) at(UI_CLICK, x, y);
        else if (kind == RADAR && phase == Phase.PANNING) at(UI_HOLD_END, x, y);
        else if (phase == Phase.DRAGGING) at(UI_DRAG_END, x, y);
        else if (phase == Phase.AIMING) at(!moved && endedAt - downAt >= LONG_HOLD_MS ? CANCEL : AIM_COMMIT, x, y);
        else if (phase == Phase.BOX) at(BOX_END, x, y);
        else if (phase == Phase.UI_HOLD || phase == Phase.SCROLLING) at(UI_HOLD_END, x, y);
        else if (phase == Phase.DONE && ui()) at(UI_HOLD_END, x, y);
        else if (phase == Phase.READY) {
            if (endedAt - downAt >= LONG_HOLD_MS) {
                if (kind == WORLD || kind == RADAR) at(HOLD, startX, startY);
                else if(kind==BOOK)at(OPEN_TOOLS,startX,startY);
                else if (ui()) { at(UI_HOLD_BEGIN, startX, startY); at(UI_HOLD_END, x, y); }
            } else if (ui()) { lastTapAt = -1; at(UI_CLICK, x, y); }
            else if (kind == WORLD) {
                boolean twice = lastTapAt >= 0 && endedAt - lastTapAt >= 0 && endedAt - lastTapAt <= DOUBLE_TAP_MS && distance(x - lastTapX, y - lastTapY) <= slop * 2;
                at(twice ? DOUBLE_TAP : TAP, x, y);
                lastTapAt = twice ? -1 : endedAt; lastTapX = x; lastTapY = y;
            }
        }
        active = false; phase = Phase.IDLE;
    }
    void secondDown(float x0, float y0, float x1, float y1, long time) {
        if (defer(() -> secondDown(x0, y0, x1, y1, time))) return;
        if (!active || ended || phase == Phase.DONE) return;
        pair = true; pairMoved = moved; pairAt = time; pairStartX = pairX = (x0 + x1) / 2; pairStartY = pairY = (y0 + y1) / 2;
        initialSpan = pairSpan = sentSpan = Math.max(1, distance(x1 - x0, y1 - y0));
        pairAngle = Math.atan2(y1 - y0, x1 - x0); twist = 0; turns = 0;
        if (kind != PENDING) beginPair();
    }
    private void beginPair() {
        lastTapAt = -1;
        if (kind == ABILITY || kind == BUILDING) { at(CANCEL, x, y); phase = Phase.DONE; return; }
        if (kind != WORLD) {
            if (phase == Phase.UI_HOLD) at(UI_HOLD_END, x, y);
            at(ABORT, x, y); phase = Phase.PAIR; return;
        }
        // Stop any selection preview without committing it or clearing the existing selection.
        if (phase == Phase.BOX) at(ABORT, x, y);
        phase = Phase.PAIR; sentX = pairStartX; sentY = pairStartY;
        pairMovement();
    }
    void pairMove(float x0, float y0, float x1, float y1) {
        if (defer(() -> pairMove(x0, y0, x1, y1))) return;
        if (!active || !pair || pairEnded || phase == Phase.DONE) return;
        pairX = (x0 + x1) / 2; pairY = (y0 + y1) / 2;
        pairSpan = Math.max(1, distance(x1 - x0, y1 - y0));
        double angle = Math.atan2(y1 - y0, x1 - x0), delta = angle - pairAngle;
        while (delta > Math.PI) delta -= 2 * Math.PI;
        while (delta < -Math.PI) delta += 2 * Math.PI;
        pairAngle = angle; twist += delta;
        if (distance(pairX - pairStartX, pairY - pairStartY) > slop || Math.abs(pairSpan - initialSpan) > slop ||
            Math.abs(twist) > TWIST_SLOP) pairMoved = true;
        if (kind != PENDING && phase == Phase.PAIR) pairMovement();
    }
    private void pairMovement() {
        if (!pairMoved || kind != WORLD) return;
        send(TRANSFORM, pairX, pairY, pairX - sentX, pairY - sentY, pairSpan / sentSpan);
        sentX = pairX; sentY = pairY; sentSpan = pairSpan;
        /* Screen y grows downwards: a growing angle is a clockwise twist. */
        while (twist >= (turns + .5) * TURN + TURN_HYSTERESIS) { turns++; send(ROTATE, pairX, pairY, 1, 0, 1); }
        while (twist <= (turns - .5) * TURN - TURN_HYSTERESIS) { turns--; send(ROTATE, pairX, pairY, -1, 0, 1); }
    }
    void pointerUp(long time) {
        if (defer(() -> pointerUp(time))) return;
        if (!active || !pair || pairEnded) return;
        pairEnded = true; endedAt = time;
        if (kind != PENDING) finishPair();
    }
    private void finishPair() {
        if (phase == Phase.PAIR && !pairMoved && endedAt - pairAt < 350) at(kind==RADAR?RADAR_TOGGLE:CANCEL, pairX, pairY);
        active = false; phase = Phase.IDLE;
    }
    void abort() {
        deferred.clear(); abortCurrent();
    }
    private void abortCurrent() {
        if (active) { at(ABORT, x, y); lastTapAt = -1; }
        active = ended = pair = pairEnded = moved = pairMoved = false;
        kind = PENDING; phase = Phase.IDLE;
    }
}
