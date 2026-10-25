package org.stronghold.port;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;

/* Crisp GDI text (native text_overlay.c): a run is laid out exactly like
 * GameActivity.textMask lays it out for the game's raster copy, then drawn
 * again at output resolution. The raster box, baseline and advance are those
 * of the original size times the scale, so the crisp run covers the same box. */
public final class TextRaster {
    private TextRaster() {}

    /* Same typeface, flags and size rule as GameActivity.textMask. */
    static Paint paint(int size, int weight, int italic) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        paint.setHinting(Paint.HINTING_ON);
        int style = (weight >= 700 ? Typeface.BOLD : 0) | (italic != 0 ? Typeface.ITALIC : 0);
        paint.setTypeface(Typeface.create("sans-serif", style));
        paint.setColor(0xffffffff);
        if (size == 0) size = 16;
        paint.setTextSize(Math.max(1, Math.min(512, Math.abs(size))));
        if (size > 0) {
            Paint.FontMetrics fm = paint.getFontMetrics();
            paint.setTextSize(size * paint.getTextSize() / (fm.bottom - fm.top));
        }
        return paint;
    }

    /* ALPHA_8 width x height image with the text origin (the raster box's
     * top-left) at dx,dy. The caller sizes it to the scaled raster box plus a
     * pixel of the original size around it: glyphs at the larger size may reach
     * a fraction of a pixel beyond the box the original rasterizer cut them to. */
    public static Bitmap render(String text, int size, int weight, int italic, float scale,
                                float dx, float dy, int width, int height) {
        if (text == null || !(scale > 0) || width <= 0 || height <= 0 || width > 8192 || height > 2048) return null;
        Paint paint = paint(size, weight, italic);
        Paint.FontMetrics fm = paint.getFontMetrics();
        float advance = paint.measureText(text);
        float baseline = -fm.top;
        paint.setTextSize(paint.getTextSize() * scale);
        /* Hinted advances are not exactly linear in size: keep the original run length. */
        float wide = paint.measureText(text);
        if (advance > 0 && wide > 0) {
            float stretch = advance * scale / wide;
            if (stretch > 0.8f && stretch < 1.25f) paint.setTextScaleX(stretch);
        }
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ALPHA_8);
        /* A whole-pixel baseline keeps hinted horizontal stems on one row. */
        new Canvas(bitmap).drawText(text, dx, Math.round(dy + baseline * scale), paint);
        return bitmap;
    }
}
