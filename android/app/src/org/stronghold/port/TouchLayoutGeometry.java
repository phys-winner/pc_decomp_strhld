package org.stronghold.port;

/** Editor rectangles use the same top-left coordinates as the saved control positions. */
final class TouchLayoutGeometry {
    static final class Rect {
        final float left, top, width, height;
        Rect(float left, float top, float width, float height) {
            this.left = left; this.top = top; this.width = width; this.height = height;
        }
    }

    private static boolean positive(float value) { return Float.isFinite(value) && value > 0; }
    private static float minimum(float value) { return positive(value) ? value : 1; }
    private static float size(double value, float viewport, float minimum) {
        float low = Math.min(viewport, minimum);
        return (float)Math.max(low, Math.min(viewport, Double.isFinite(value) ? value : low));
    }
    private static float position(float value, float maximum) {
        return Math.max(0, Math.min(maximum, Float.isFinite(value) ? value : 0));
    }

    static Rect fit(float left, float top, float width, float height,
                    float viewWidth, float viewHeight, float minPixels) {
        if (!positive(viewWidth) || !positive(viewHeight)) return new Rect(0, 0, 0, 0);
        float min = minimum(minPixels);
        float w = size(width, viewWidth, min), h = size(height, viewHeight, min);
        return new Rect(position(left, viewWidth - w), position(top, viewHeight - h), w, h);
    }

    static Rect scaleAboutCenter(Rect rect, float scaleX, float scaleY,
                                 float viewWidth, float viewHeight, float minPixels) {
        if (rect == null) return fit(0, 0, minPixels, minPixels, viewWidth, viewHeight, minPixels);
        Rect base = fit(rect.left, rect.top, rect.width, rect.height, viewWidth, viewHeight, minPixels);
        if (base.width == 0 || base.height == 0) return base;
        float w = size((double)base.width * (positive(scaleX) ? scaleX : 1), viewWidth, minimum(minPixels));
        float h = size((double)base.height * (positive(scaleY) ? scaleY : 1), viewHeight, minimum(minPixels));
        return fit(base.left + (base.width - w) / 2, base.top + (base.height - h) / 2,
                w, h, viewWidth, viewHeight, minPixels);
    }

    static float[] commandPopupBand(float zoneTop, float zoneBottom, float contentHeight,
                                    float rowHeight, float viewHeight, float margin) {
        if (!positive(viewHeight) || !Float.isFinite(zoneTop) || !Float.isFinite(zoneBottom) || zoneBottom < zoneTop)
            return new float[]{0, 0};
        float top = position(zoneTop, viewHeight), bottom = position(zoneBottom, viewHeight);
        float gap = positive(margin) ? Math.min(margin, viewHeight / 2) : 0;
        float above = Math.max(0, top - gap - gap), belowTop = Math.min(viewHeight, bottom + gap);
        float below = Math.max(0, viewHeight - gap - belowTop);
        float wanted = positive(contentHeight) ? contentHeight : 0;
        boolean placeBelow = above < Math.min(wanted, minimum(rowHeight)) && below > above;
        float height = Math.min(wanted, placeBelow ? below : above);
        return new float[]{placeBelow ? belowTop : Math.max(0, top - gap - height), height};
    }

    static float chooseToolbarTop(float selectionTop, float selectionBottom, float toolbarHeight,
                                  float viewHeight, float margin) {
        if (!positive(viewHeight)) return 0;
        float height = positive(toolbarHeight) ? Math.min(toolbarHeight, viewHeight) : 0;
        float gap = positive(margin) ? Math.min(margin, (viewHeight - height) / 2) : 0;
        double center = Float.isFinite(selectionTop) && Float.isFinite(selectionBottom)
                ? ((double)selectionTop + selectionBottom) / 2 : viewHeight / 2;
        return center >= viewHeight / 2 ? gap : viewHeight - height - gap;
    }
}
