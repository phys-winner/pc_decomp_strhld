package org.stronghold.port;

/** Touch arbitration is independent of Android so cancellation can be tested on a PC. */
final class TouchControls {
    interface Output {
        void move(float x, float y, boolean warp);
        void button(int button, boolean down);
        void click(int button);
        void key(int scan, boolean down);
        void cursor(float x, float y);
    }
    private final Output out;
    private final float slop;
    private boolean pad, single, multi, ignored, drag, consumed, moved;
    private float sensitivity = 1, left, top, right = 1, bottom = 1;
    private float startX, startY, lastX, lastY, pairX, pairY, pairSpan;
    private float cursorX, cursorY;
    private long downAt, pairAt;
    private int arrows;

    TouchControls(Output output, float touchSlop) { out = output; slop = touchSlop; }
    void bounds(float l, float t, float r, float b) {
        left = l; top = t; right = r; bottom = b;
        cursorX = clamp(cursorX == 0 ? (l + r) / 2 : cursorX, l, r);
        cursorY = clamp(cursorY == 0 ? (t + b) / 2 : cursorY, t, b);
        if (pad) point(cursorX, cursorY, true); else out.cursor(cursorX, cursorY);
    }
    void mode(boolean touchpad, float speed) { cancel(); pad = touchpad; sensitivity = speed; }
    private static float clamp(float x, float lo, float hi) { return Math.max(lo, Math.min(hi, x)); }
    private static float distance(float x, float y) { return (float)Math.hypot(x, y); }
    private void point(float x, float y, boolean warp) {
        cursorX = clamp(x, left, right); cursorY = clamp(y, top, bottom);
        out.move(cursorX, cursorY, warp); out.cursor(cursorX, cursorY);
    }
    void syncCursor() { point(cursorX, cursorY, true); }
    /** The touchpad cursor continues from where a physical mouse left it. */
    void place(float x, float y) { cursorX = clamp(x, left, right); cursorY = clamp(y, top, bottom); out.cursor(cursorX, cursorY); }
    void down(float x, float y, long time) {
        cancel();
        if (x < left || x > right || y < top || y > bottom) { ignored = true; return; }
        single = true; startX = lastX = x; startY = lastY = y; downAt = time;
        if (!pad) point(x, y, true);
    }
    void move(float x, float y) {
        if (!single || ignored) return;
        float displacement = distance(x - startX, y - startY);
        if (pad) {
            if (displacement > slop) moved = true;
            if (moved) point(cursorX + (x - lastX) * sensitivity, cursorY + (y - lastY) * sensitivity, false);
        } else if (!consumed && displacement > slop) {
            if (!drag) { point(startX, startY, true); out.button(0, true); drag = true; }
            point(x, y, false); moved = true;
        }
        lastX = x; lastY = y;
    }
    void longPress(long time) {
        if (single && !ignored && !drag && !moved && !consumed && time - downAt >= 500) {
            if (!pad) point(startX, startY, true); else syncCursor();
            out.click(1); consumed = true;
        }
    }
    void up(float x, float y, long time) {
        if (ignored) { cancel(); return; }
        if (single) {
            longPress(time);
            if (!drag && !consumed && !moved) {
                if (!pad) point(x, y, true); else syncCursor();
                out.click(0);
            }
        }
        cancel();
    }
    void secondDown(float x0, float y0, float x1, float y1, long time) {
        if (ignored) return;
        if (drag) { out.button(0, false); drag = false; }
        single = false; multi = true; moved = consumed; pairAt = time;
        pairX = (x0 + x1) / 2; pairY = (y0 + y1) / 2;
        pairSpan = distance(x1 - x0, y1 - y0);
    }
    void pairMove(float x0, float y0, float x1, float y1) {
        if (!multi || ignored) return;
        float dx = (x0 + x1) / 2 - pairX, dy = (y0 + y1) / 2 - pairY;
        // A pinch is not a two-finger tap even when its centroid stays still.
        if (distance(dx, dy) > slop || Math.abs(distance(x1 - x0, y1 - y0) - pairSpan) > slop) moved = true;
        int next = 0;
        if (Math.abs(dx) > slop * 1.5f) next |= dx > 0 ? 1 : 2;
        if (Math.abs(dy) > slop * 1.5f) next |= dy > 0 ? 4 : 8;
        arrows(next);
    }
    void pointerUp(long time) {
        if (multi && !ignored && !moved && time - pairAt < 350) { syncCursor(); out.click(1); }
        arrows(0); multi = single = false; ignored = true;
    }
    void cancel() {
        if (drag) out.button(0, false);
        arrows(0); single = multi = ignored = drag = consumed = moved = false;
    }
    private void arrows(int next) {
        int[] scans = {0xcb, 0xcd, 0xc8, 0xd0};
        for (int i = 0; i < scans.length; ++i) if (((arrows ^ next) & (1 << i)) != 0)
            out.key(scans[i], (next & (1 << i)) != 0);
        arrows = next;
    }

    static int[] worldResolution(int width, int height, boolean wide) {
        int w = Math.max(width, height), h = Math.max(1, Math.min(width, height));
        double aspect = Math.max(4.0 / 3, Math.min(3, wide ? (double)w / h : 16.0 / 9));
        w = (int)Math.round(h * aspect);
        double fit = Math.min(1, Math.min(4056.0 / w, 2076.0 / h));
        h = Math.max(600, (int)Math.floor(h * fit));
        w = Math.max(800, Math.min(Math.min(4056, h * 3) & ~7, ((int)Math.round(h * aspect) + 7) & ~7));
        return new int[] {w, h};
    }

    static int[] resolution(String selected, int deviceWidth, int deviceHeight, float density, boolean wide) {
        int height = Math.min(deviceWidth, deviceHeight);
        if ("auto".equals(selected) || "autoplus".equals(selected)) { /* Авто+ starts from Авто */
            if (density >= 2) height /= 2;
        } else if ("minimum".equals(selected)) {
            height = 600; /* The engine's smallest frame height; the width follows the chosen aspect. */
        } else if (!"device".equals(selected)) {
            try { height = Integer.parseInt(selected.split("x")[1]); }
            catch (RuntimeException ignored) { /* Invalid saved settings use the device size. */ }
        }
        double aspect = wide ? (double)Math.max(deviceWidth, deviceHeight) / Math.max(1, Math.min(deviceWidth, deviceHeight)) : 16.0 / 9;
        return worldResolution((int)Math.round(Math.max(600, height) * aspect), Math.max(600, height), true);
    }

    /* Авто+: frame heights it steps through, one level per game start. A level
     * is left upwards once its 95th-percentile frame cost (frame start to
     * submission; 40 fps leaves 25 ms) stays comfortably low over two minutes
     * of match frames, and downwards once it exceeds the budget's margin. */
    static final int[] AUTO_PLUS_HEIGHTS = {600, 720, 864, 1080, 1296, 1440, 1728, 2076};
    static final float AUTO_PLUS_UP_MS = 15, AUTO_PLUS_DOWN_MS = 23;
    static final int AUTO_PLUS_UP_FRAMES = 4800, AUTO_PLUS_DOWN_FRAMES = 1200, AUTO_PLUS_MEMORY = 24000;

    /** Heights from the engine minimum up to the device's own (capped by the
     * engine), always including the "Авто" height where Авто+ begins. */
    static int[] autoPlusLevels(int deviceWidth, int deviceHeight, float density) {
        int auto = resolution("auto", deviceWidth, deviceHeight, density, true)[1];
        int top = Math.max(auto, Math.min(2076, Math.min(deviceWidth, deviceHeight)));
        java.util.TreeSet<Integer> levels = new java.util.TreeSet<>();
        levels.add(auto); levels.add(top);
        for (int height : AUTO_PLUS_HEIGHTS) if (height <= top) levels.add(height);
        int[] result = new int[levels.size()]; int at = 0;
        for (int height : levels) result[at++] = height;
        return result;
    }
    static int autoPlusStart(int[] levels, int deviceWidth, int deviceHeight, float density) {
        int auto = resolution("auto", deviceWidth, deviceHeight, density, true)[1];
        for (int i = 0; i < levels.length; i++) if (levels[i] == auto) return i;
        return 0;
    }
    /** The level for the next start, from the one used last and each level's
     * recorded p95 (ms) and sampled match frames (0 = never measured). A level
     * already measured as too slow is not entered again, which keeps the
     * choice from oscillating between two neighbours. */
    static int autoPlusStep(int current, float[] p95, int[] frames) {
        if (current < 0 || current >= p95.length) return 0;
        if (frames[current] >= AUTO_PLUS_DOWN_FRAMES && p95[current] > AUTO_PLUS_DOWN_MS) return Math.max(0, current - 1);
        int up = current + 1;
        if (up < p95.length && frames[current] >= AUTO_PLUS_UP_FRAMES && p95[current] < AUTO_PLUS_UP_MS &&
                !(frames[up] >= AUTO_PLUS_DOWN_FRAMES && p95[up] > AUTO_PLUS_DOWN_MS)) return up;
        return current;
    }
    /** Folds a sampled window into a level's record {p95 ms, frames}: a
     * frame-weighted mean that gradually forgets beyond AUTO_PLUS_MEMORY frames. */
    static float[] autoPlusRecord(float p95, int frames, float sampleP95, int sampleFrames) {
        if (sampleFrames <= 0 || !(sampleP95 >= 0) || Float.isInfinite(sampleP95)) return new float[] {p95, frames};
        float weight = Math.min(Math.max(0, frames), AUTO_PLUS_MEMORY);
        float mean = (p95 * weight + sampleP95 * sampleFrames) / (weight + sampleFrames);
        return new float[] {mean, (float)Math.min((long)Math.max(0, frames) + sampleFrames, Integer.MAX_VALUE)};
    }

    static int radialSector(float x, float y, float cx, float cy, float radius) {
        double distance = Math.hypot(x - cx, y - cy);
        if (!Double.isFinite(distance) || !(radius > 0) || distance < radius * .45 || distance > radius * 1.65) return -1;
        double angle = Math.atan2(y - cy, x - cx) + Math.PI / 2 + Math.PI / 9;
        if (angle < 0) angle += Math.PI * 2;
        return (int)Math.floor(angle / (Math.PI * 2 / 9)) % 9;
    }
}
