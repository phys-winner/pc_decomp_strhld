package org.stronghold.port;

public final class TouchLayoutGeometrySelfcheck {
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static void near(float actual, float expected) {
        check(Float.isFinite(actual) && Math.abs(actual - expected) < .001f, actual + " != " + expected);
    }
    private static void rect(TouchLayoutGeometry.Rect r, float x, float y, float w, float h) {
        near(r.left, x); near(r.top, y); near(r.width, w); near(r.height, h);
    }
    public static void main(String[] args) {
        rect(TouchLayoutGeometry.fit(10, 20, 80, 60, 1000, 600, 24), 10, 20, 80, 60);
        rect(TouchLayoutGeometry.fit(-30, 590, 80, 60, 1000, 600, 24), 0, 540, 80, 60);
        rect(TouchLayoutGeometry.fit(999, -1, 1, 2, 1000, 600, 24), 976, 0, 24, 24);
        rect(TouchLayoutGeometry.fit(20, 20, 2000, 900, 1000, 600, 24), 0, 0, 1000, 600);
        rect(TouchLayoutGeometry.fit(20, 20, 2000, 900, 10, 8, 24), 0, 0, 10, 8);
        rect(TouchLayoutGeometry.fit(Float.NaN, Float.POSITIVE_INFINITY, Float.NaN, Float.NEGATIVE_INFINITY,
                1000, 600, 24), 0, 0, 24, 24);
        rect(TouchLayoutGeometry.fit(0, 0, 0, 0, 1000, 600, Float.NaN), 0, 0, 1, 1);
        for (float invalid : new float[]{0, -1, Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY}) {
            rect(TouchLayoutGeometry.fit(1, 1, 40, 40, invalid, 600, 24), 0, 0, 0, 0);
            rect(TouchLayoutGeometry.fit(1, 1, 40, 40, 1000, invalid, 24), 0, 0, 0, 0);
        }

        TouchLayoutGeometry.Rect original = new TouchLayoutGeometry.Rect(400, 200, 100, 80);
        rect(TouchLayoutGeometry.scaleAboutCenter(original, 2, .5f, 1000, 600, 24), 350, 220, 200, 40);
        rect(TouchLayoutGeometry.scaleAboutCenter(original, .01f, .01f, 1000, 600, 24), 438, 228, 24, 24);
        rect(TouchLayoutGeometry.scaleAboutCenter(original, Float.MAX_VALUE, Float.MAX_VALUE, 1000, 600, 24),
                0, 0, 1000, 600);
        for (float invalid : new float[]{0, -1, Float.NaN, Float.POSITIVE_INFINITY})
            rect(TouchLayoutGeometry.scaleAboutCenter(original, invalid, invalid, 1000, 600, 24), 400, 200, 100, 80);
        rect(TouchLayoutGeometry.scaleAboutCenter(new TouchLayoutGeometry.Rect(0, 0, 40, 40), 2, 2,
                1000, 600, 24), 0, 0, 80, 80);
        rect(TouchLayoutGeometry.scaleAboutCenter(original, 2, 2, 0, 600, 24), 0, 0, 0, 0);
        rect(TouchLayoutGeometry.scaleAboutCenter(null, 2, 2, 1000, 600, 24), 0, 0, 24, 24);

        float[][] popups = {
                {500, 600, 200, 48, 600, 4, 296, 200},
                {0, 100, 200, 48, 600, 4, 104, 200},
                {20, 100, 700, 48, 600, 4, 104, 492},
                {100, 200, 300, 48, 600, 4, 4, 92},
                {30, 580, 100, 48, 600, 4, 4, 22},
                {0, 600, 200, 48, 600, 4, 0, 0},
                {-20, 100, 200, 48, 600, 4, 104, 200},
                {500, 900, 200, 48, 600, 4, 296, 200},
                {0, 100, 200, 48, 600, 0, 100, 200},
                {0, 100, 0, 48, 600, 4, 0, 0},
                {20, 100, 200, 48, 0, 4, 0, 0},
                {Float.NaN, 100, 200, 48, 600, 4, 0, 0},
                {200, 100, 200, 48, 600, 4, 0, 0}
        };
        for (float[] c : popups) {
            float[] band = TouchLayoutGeometry.commandPopupBand(c[0], c[1], c[2], c[3], c[4], c[5]);
            near(band[0], c[6]); near(band[1], c[7]);
        }
        for (int top = 0; top <= 600; top += 20) for (int bottom = top; bottom <= 600; bottom += 20)
            for (float wanted : new float[]{0, 24, 48, 200, Float.MAX_VALUE}) {
                float[] band = TouchLayoutGeometry.commandPopupBand(top, bottom, wanted, 48, 600, 4);
                check(band[0] >= 0 && band[1] >= 0 && band[0] + band[1] <= 600, "Popup stays in viewport");
                check(band[1] == 0 || band[0] + band[1] <= top - 4 || band[0] >= bottom + 4,
                        "Popup does not overlap its zone");
                if (top - 8 >= Math.min(wanted, 48))
                    check(band[0] + band[1] <= top - 4, "Prefer above when a command fits");
            }

        near(TouchLayoutGeometry.chooseToolbarTop(20, 60, 120, 600, 8), 472);
        near(TouchLayoutGeometry.chooseToolbarTop(480, 580, 120, 600, 8), 8);
        near(TouchLayoutGeometry.chooseToolbarTop(280, 320, 120, 600, 8), 8);
        near(TouchLayoutGeometry.chooseToolbarTop(Float.NaN, 320, 120, 600, 8), 8);
        near(TouchLayoutGeometry.chooseToolbarTop(0, 20, 900, 600, 8), 0);
        near(TouchLayoutGeometry.chooseToolbarTop(0, 20, 120, 600, 900), 240);
        near(TouchLayoutGeometry.chooseToolbarTop(0, 20, 120, 0, 8), 0);
        near(TouchLayoutGeometry.chooseToolbarTop(0, 20, 120, Float.NaN, 8), 0);
        near(TouchLayoutGeometry.chooseToolbarTop(480, 580, 120, 600, Float.POSITIVE_INFINITY), 0);
        System.out.println("Touch layout geometry: finite bounds, minimums, centered scaling, popup bands and toolbar avoidance PASS");
    }
}
