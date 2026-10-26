import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import java.lang.reflect.Method;

/** Shell-only QA input: paired diagonal camera pans and a centered pinch. */
public final class GameplayInput {
    private static Object manager;
    private static Method inject;

    private static float[] shiftPoints(int width, int height, double dx, double dy) {
        if (!Double.isFinite(dx) || !Double.isFinite(dy) || Math.abs(dx) > width * .3 || Math.abs(dy) > height * .3)
            throw new IllegalArgumentException("Shift must be finite and within 30% of screen dimensions");
        return new float[] {(float)(width * .5 - dx / 2), (float)(height * .35 - dy / 2),
                (float)(width * .5 + dx / 2), (float)(height * .35 + dy / 2)};
    }

    private static void mouseEvent(long down, int action, int buttons, float x, float y) throws Exception {
        MotionEvent.PointerProperties pointer = new MotionEvent.PointerProperties();
        pointer.id = 0;
        pointer.toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords coordinates = new MotionEvent.PointerCoords();
        coordinates.x = x;
        coordinates.y = y;
        coordinates.pressure = buttons != 0 ? 1 : 0;
        MotionEvent motion = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, 1,
                new MotionEvent.PointerProperties[] {pointer}, new MotionEvent.PointerCoords[] {coordinates},
                0, buttons, 1, 1, 0, 0, InputDevice.SOURCE_MOUSE, 0);
        try {
            if (!(Boolean) inject.invoke(manager, motion, 2)) throw new IllegalStateException("Mouse input rejected: " + action);
        } finally {
            motion.recycle();
        }
    }

    private static void click(float x, float y) throws Exception {
        long down = SystemClock.uptimeMillis();
        Exception error = null;
        try {
            mouseEvent(down, MotionEvent.ACTION_DOWN, MotionEvent.BUTTON_PRIMARY, x, y);
            SystemClock.sleep(100);
        } catch (Exception failure) {
            error = failure;
            try { mouseEvent(down, MotionEvent.ACTION_CANCEL, 0, x, y); }
            catch (Exception cleanup) { failure.addSuppressed(cleanup); }
            throw failure;
        } finally {
            try { mouseEvent(down, MotionEvent.ACTION_UP, 0, x, y); }
            catch (Exception release) { if (error != null) error.addSuppressed(release); else throw release; }
        }
    }

    private static void event(long down, int action, float x, float y, float span, int count) throws Exception {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[count];
        for (int i = 0; i < count; i++) {
            properties[i] = new MotionEvent.PointerProperties();
            properties[i].id = i;
            properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coordinates[i] = new MotionEvent.PointerCoords();
            coordinates[i].x = x + (i == 0 ? -span / 2 : span / 2);
            coordinates[i].y = y;
            coordinates[i].pressure = 1;
            coordinates[i].size = 1;
        }
        MotionEvent motion = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, count,
                properties, coordinates, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        try {
            if (!(Boolean) inject.invoke(manager, motion, 2)) throw new IllegalStateException("Input rejected: " + action);
        } finally {
            motion.recycle();
        }
    }

    private static void gesture(float x0, float y0, float x1, float y1,
                                float span0, float span1, long duration) throws Exception {
        long down = SystemClock.uptimeMillis();
        event(down, MotionEvent.ACTION_DOWN, x0, y0, span0, 1);
        boolean paired = false;
        float x = x0, y = y0, span = span0;
        try {
            event(down, MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), x, y, span, 2);
            paired = true;
            long start = SystemClock.uptimeMillis();
            for (int step = 1; ; step++) {
                long target = start + Math.min(duration, step * 16L);
                SystemClock.sleep(Math.max(0, target - SystemClock.uptimeMillis()));
                float progress = Math.min(1, (SystemClock.uptimeMillis() - start) / (float) duration);
                x = x0 + (x1 - x0) * progress;
                y = y0 + (y1 - y0) * progress;
                span = span0 + (span1 - span0) * progress;
                event(down, MotionEvent.ACTION_MOVE, x, y, span, 2);
                if (progress >= 1) break;
            }
            event(down, MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), x, y, span, 2);
            paired = false;
            event(down, MotionEvent.ACTION_UP, x, y, span, 1);
        } catch (Exception failure) {
            event(down, MotionEvent.ACTION_CANCEL, x, y, span, paired ? 2 : 1);
            throw failure;
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 3) throw new IllegalArgumentException("pan width height seconds rounds | pinch width height factor milliseconds | shift width height dx dy | hover/click width height x y | selfcheck width height");
        int width = Integer.parseInt(args[1]), height = Integer.parseInt(args[2]);
        if (width < 320 || height < 240 || width > 16384 || height > 16384)
            throw new IllegalArgumentException("Screen dimensions out of range");
        float[] x = {width * .5f, width * .62f, width * .5f, width * .38f, width * .5f};
        float[] y = {height * .24f, height * .35f, height * .46f, height * .35f, height * .24f};
        for (int i = 0; i < x.length; i++)
            if (x[i] - width * .05f <= 0 || x[i] + width * .05f >= width || y[i] <= 0 || y[i] >= height * .6f)
                throw new AssertionError("Gesture left the world viewport");
        if ("selfcheck".equals(args[0])) {
            if (x[0] != x[4] || y[0] != y[4]) throw new AssertionError("Pan is not closed");
            for (int sign : new int[] {-1, 1}) {
                float[] points = shiftPoints(width, height, sign * width * .3, sign * height * .3);
                if (Math.abs(points[2] - points[0] - sign * width * .3) > .01 ||
                        Math.abs(points[3] - points[1] - sign * height * .3) > .01 ||
                        points[0] <= width * .05 || points[2] >= width * .95 || points[1] <= 0 || points[3] >= height * .6)
                    throw new AssertionError("Shift geometry is invalid");
            }
            System.out.println("GameplayInput geometry PASS");
            return;
        }
        if (args.length != 5) throw new IllegalArgumentException("Expected five arguments");
        double value = Double.parseDouble(args[3]);
        boolean shift = "shift".equals(args[0]);
        boolean hover = "hover".equals(args[0]);
        boolean click = "click".equals(args[0]);
        boolean mouse = hover || click;
        double other = Double.parseDouble(args[4]);
        float[] shiftPath = shift ? shiftPoints(width, height, value, Double.parseDouble(args[4])) : null;
        int count = shift || mouse ? 0 : Integer.parseInt(args[4]);
        if (mouse && (!Double.isFinite(value) || !Double.isFinite(other) || value < 0 || value >= width || other < 0 || other >= height))
            throw new IllegalArgumentException("Mouse coordinates must be finite and inside the screen");
        if (!shift && !mouse && (!Double.isFinite(value) || ("pan".equals(args[0]) ? value < 1 || value > 600 || count < 1 || count > 100 || value * 1000 < count * 200 :
                !"pinch".equals(args[0]) || value < .1 || value > 2 || count < 100 || count > 10000)))
            throw new IllegalArgumentException("Invalid gesture duration, rounds or pinch factor");
        Class<?> type = Class.forName("android.hardware.input.InputManager");
        manager = type.getMethod("getInstance").invoke(null);
        inject = type.getMethod("injectInputEvent", InputEvent.class, int.class);
        long before = SystemClock.uptimeMillis();
        if (click)
            click((float)value, (float)other);
        else if (hover)
            mouseEvent(before, MotionEvent.ACTION_HOVER_MOVE, 0, (float)value, (float)other);
        else if (shift)
            gesture(shiftPath[0], shiftPath[1], shiftPath[2], shiftPath[3], width * .1f, width * .1f, 500);
        else if ("pinch".equals(args[0]))
            gesture(width * .5f, height * .35f, width * .5f, height * .35f, width * .32f, (float)(width * .32f * value), count);
        else {
            long leg = Math.max(50, Math.round(value * 1000 / (count * 4)));
            for (int round = 0; round < count; round++) for (int side = 0; side < 4; side++)
                gesture(x[side], y[side], x[side + 1], y[side + 1], width * .1f, width * .1f, leg);
        }
        System.out.println("{\"action\":\"" + args[0] + "\",\"elapsed_ms\":" + (SystemClock.uptimeMillis() - before) + "}");
    }
}
