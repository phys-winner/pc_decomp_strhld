package port.launcher;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.view.WindowInsets;
import java.util.function.DoubleSupplier;
import java.util.function.Supplier;

/** The presented-frame FPS counter and graph, drawn over the game's surface without taking touches. */
public final class FpsOverlay extends View {
    public interface Source {
        /**
         * Fills the array with recent intervals between presented game frames in microseconds,
         * oldest first and padded with zeros on the left. Returns the cumulative interval count.
         */
        long read(int[] intervalsUs);
    }

    /** Returns null when the player leaves the overlay off. */
    public static FpsOverlay create(Activity activity, Source frames) {
        for (Spec.Option option : Spec.get(activity).options) {
            if ("fps_counter".equals(option.key))
                return create(activity, Launch.option(activity, option), frames,
                    () -> activity.getIntent().getStringExtra(Launch.EXTRA_RENDERER), () -> 0, null);
        }
        return null;
    }

    /**
     * Uses the actual renderer ID and live FPS limit when the port can provide them.
     * The optional GPU supplier returns the device and driver strings separated by a newline.
     * System suppliers run off the UI thread.
     */
    public static FpsOverlay create(Context context, String mode, Source frames,
                                    Supplier<String> renderer, DoubleSupplier fpsLimit, Supplier<String> gpu) {
        if (!"1".equals(mode) && !"2".equals(mode)) return null;
        int mask = HudField.read(context, mode);
        SystemInfo system = (mask & ~HudField.FRAMES) != 0 ? new SystemInfo(context, mask, renderer, fpsLimit, gpu) : null;
        return new FpsOverlay(context, frames, mask, system);
    }

    private static final long REDRAW_MS = 50, SYSTEM_MS = 1000;
    /** System lines: label and value columns, in the 12-unit text of DXVK's min/max line. */
    private static final float LINE = 15, VALUE_X = 91;
    private static final int GRAPH_HEIGHT = 40;

    private int graphScale = 60;
    private final Source source;
    private final SystemInfo system;
    private final int mask;
    private final float unit;
    private final int[] locationInWindow = new int[2];
    private final int[] intervals = new int[FpsCounter.POINTS];
    private final int[] graphPixels;
    private final Bitmap graph;
    private final Rect graphSource = new Rect(0, 0, FpsCounter.POINTS, GRAPH_HEIGHT);
    private final RectF graphTarget = new RectF();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), outline = new Paint(Paint.ANTI_ALIAS_FLAG), pixels = new Paint();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private HandlerThread sampler;
    private volatile Handler samplerHandler;
    private volatile String[][] lines = new String[0][];
    private long sampleFrames = -1, sampleUs, min, max;
    private String frameRate = FpsCounter.frameRate(0, 0);
    private boolean running;
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            refresh();
            if (running) handler.postDelayed(this, (mask & HudField.FRAMES) != 0 ? REDRAW_MS : FpsCounter.UPDATE_US / 1000);
        }
    };
    private final Runnable sampleSystem = new Runnable() {
        @Override public void run() {
            Handler h = samplerHandler;
            if (h == null || h.getLooper() != Looper.myLooper()) return;
            try {
                String[][] latest = system.sample();
                if (h == samplerHandler) lines = latest;
            } catch (RuntimeException e) {
                android.util.Log.w("Launcher", "FPS counter system lines", e);
            }
            if (h == samplerHandler) h.postDelayed(this, SYSTEM_MS);
        }
    };

    private FpsOverlay(Context context, Source source, int mask, SystemInfo system) {
        super(context);
        this.unit = FpsCounter.hudScale(getResources().getDisplayMetrics().density);
        this.source = source;
        this.system = system;
        this.mask = mask;
        graph = HudField.GRAPH.in(mask) ? Bitmap.createBitmap(FpsCounter.POINTS, GRAPH_HEIGHT, Bitmap.Config.ARGB_8888) : null;
        graphPixels = graph != null ? new int[FpsCounter.POINTS * GRAPH_HEIGHT] : null;
        Typeface mono = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD);
        fill.setTypeface(mono);
        outline.setTypeface(mono);
        outline.setStyle(Paint.Style.STROKE);
        outline.setStrokeJoin(Paint.Join.ROUND);
        outline.setColor(0xFF000000);
        setClickable(false);
        setFocusable(false);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
    }

    public void start() {
        if (running) return;
        running = true;
        sampleFrames = -1;
        frameRate = FpsCounter.frameRate(0, 0);
        handler.post(tick);
        if (system != null && sampler == null) {
            sampler = new HandlerThread("fps-counter");
            sampler.start();
            samplerHandler = new Handler(sampler.getLooper());
            samplerHandler.post(sampleSystem);
        }
    }

    public void stop() {
        running = false;
        handler.removeCallbacks(tick);
        if (sampler != null) {
            Handler h = samplerHandler;
            samplerHandler = null;
            h.removeCallbacks(sampleSystem);
            sampler.quitSafely();
            sampler = null;
        }
    }

    @Override protected void onDetachedFromWindow() {
        stop();
        super.onDetachedFromWindow();
    }

    private void refresh() {
        long now = SystemClock.elapsedRealtimeNanos() / 1000, frames = source.read(intervals);
        if (sampleFrames < 0 || frames < sampleFrames) {
            sampleFrames = frames;
            sampleUs = now;
        } else if (now - sampleUs >= FpsCounter.UPDATE_US) {
            frameRate = FpsCounter.frameRate(frames - sampleFrames, now - sampleUs);
            sampleFrames = frames;
            sampleUs = now;
        }
        if (graph != null && getDisplay() != null) graphScale = Math.max(1, Math.round(getDisplay().getRefreshRate()));
        if ((mask & HudField.FRAMES) != 0) refreshGraph();
        invalidate();
    }

    private void refreshGraph() {
        long low = Long.MAX_VALUE, high = 0;
        for (int i = 0; i < FpsCounter.POINTS; ++i) {
            int us = intervals[i];
            if (us > 0) {
                long tenths = FpsCounter.tenths(us);
                low = Math.min(low, tenths);
                high = Math.max(high, tenths);
            }
            if (graph == null) continue;
            int top = us > 0 ? GRAPH_HEIGHT - Math.max(1, Math.round(GRAPH_HEIGHT * FpsCounter.fpsHeight(us, graphScale))) : GRAPH_HEIGHT;
            int color = FpsCounter.color(us);
            for (int y = 0; y < GRAPH_HEIGHT; ++y) graphPixels[y * FpsCounter.POINTS + i] = y >= top ? color : 0;
        }
        if (graph != null) graph.setPixels(graphPixels, 0, FpsCounter.POINTS, 0, 0, FpsCounter.POINTS, GRAPH_HEIGHT);
        min = low == Long.MAX_VALUE ? 0 : low;
        max = high;
    }

    private void moveIntoSafeArea(Canvas canvas) {
        WindowInsets insets = getRootWindowInsets();
        if (insets == null) return;
        int left, top;
        if (Build.VERSION.SDK_INT >= 30) {
            Insets safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            left = safe.left;
            top = safe.top;
        } else {
            left = insets.getSystemWindowInsetLeft();
            top = insets.getSystemWindowInsetTop();
        }
        getLocationInWindow(locationInWindow);
        canvas.translate(Math.max(0, left - locationInWindow[0]), Math.max(0, top - locationInWindow[1]));
    }

    /** DXVK's HudRenderer::drawText: size in HUD units, (x, y) the baseline's start, one character every
     *  0.75 of the size (its font: advance 24 at size 32) by letter spacing, glyphs in a black outline. */
    private void text(Canvas canvas, String value, float x, float y, float size, int color) {
        float px = size * unit;
        if (fill.getTextSize() != px) {
            fill.setTextSize(px);
            fill.setLetterSpacing(0);
            float spacing = 0.75f - fill.measureText("0") / px;
            fill.setLetterSpacing(spacing);
            outline.setTextSize(px);
            outline.setLetterSpacing(spacing);
            outline.setStrokeWidth(px * 0.12f);
        }
        fill.setColor(color);
        canvas.drawText(value, x * unit, y * unit, outline);
        canvas.drawText(value, x * unit, y * unit, fill);
    }

    @Override protected void onDraw(Canvas canvas) {
        int save = canvas.save();
        moveIntoSafeArea(canvas);
        float x = 8, y = 8;
        /* HudFpsItem::render */
        y += 16;
        text(canvas, "FPS:", x, y, 16, FpsCounter.LABEL);
        text(canvas, frameRate, x + 60, y, 16, FpsCounter.VALUE);
        if (graph != null) {
            y += 8;
            graphTarget.set(x * unit, y * unit, Math.min((x + FpsCounter.POINTS) * unit, getWidth() - x * unit), (y + GRAPH_HEIGHT) * unit);
            canvas.drawBitmap(graph, graphSource, graphTarget, pixels);
            y += GRAPH_HEIGHT;
            text(canvas, "0–" + graphScale + " FPS", x + 2, y - 2, 9, FpsCounter.VALUE);
        }
        if (HudField.RANGE.in(mask)) {
            y += LINE;
            text(canvas, "min / max", x, y, 12, FpsCounter.LABEL);
            text(canvas, FpsCounter.milliseconds(min) + " / " + FpsCounter.milliseconds(max) + " ms",
                x + VALUE_X, y, 12, FpsCounter.VALUE);
        }
        y += 4;
        /* The system lines, in the style of DXVK's other items */
        for (String[] line : lines) {
            y += LINE;
            text(canvas, line[0], x, y, 12, FpsCounter.LABEL);
            text(canvas, line[1], x + VALUE_X, y, 12, FpsCounter.VALUE);
        }
        canvas.restoreToCount(save);
    }
}
