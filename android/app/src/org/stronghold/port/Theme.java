package org.stronghold.port;

import android.animation.ValueAnimator;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.os.SystemClock;
import android.provider.Settings;
import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowInsets;
import android.os.Build;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Shared look of the Android screens: dark surfaces, gold accent, transparent system bars. */
final class Theme {
    static final int BACKGROUND = 0xff0f151c, SURFACE = 0xf01a232d, SURFACE_HIGH = 0xff26313d, OUTLINE = 0x2effffff,
            ACCENT = 0xffe7b45c, ON_ACCENT = 0xff21180a, TEXT = 0xffeef2f6, MUTED = 0xffa7b3c0,
            GOOD = 0xff9fd59a, BAD = 0xffffa08c;
    private Theme() {}

    static int dp(Context context, float value) { return Math.round(value * context.getResources().getDisplayMetrics().density); }

    /* The gesture handle and status bar float over the page: the content draws behind
     * them and pads itself with the system-bar insets. */
    static void transparentBars(Activity activity) {
        Window window = activity.getWindow();
        /* These screens are already dark: a vendor "force dark" pass (MIUI) would
         * darken the gold accent and lighten its dark label. */
        if (Build.VERSION.SDK_INT >= 29) window.getDecorView().setForceDarkAllowed(false);
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setNavigationBarDividerColor(Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= 29) {
            window.setStatusBarContrastEnforced(false);
            window.setNavigationBarContrastEnforced(false);
        }
        window.setBackgroundDrawable(backdrop());
    }

    static void edgeToEdge(Activity activity) {
        transparentBars(activity);
        Window window = activity.getWindow();
        if (Build.VERSION.SDK_INT >= 30) window.setDecorFitsSystemWindows(false);
        else window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        WindowManager.LayoutParams attributes = window.getAttributes();
        attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(attributes);
    }

    static WindowInsets padInsets(View target, WindowInsets insets, int left, int top, int right, int bottom) {
        if (Build.VERSION.SDK_INT >= 30) {
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            target.setPadding(left + bars.left, top + bars.top, right + bars.right, bottom + bars.bottom);
            return WindowInsets.CONSUMED;
        }
        android.view.DisplayCutout cutout = insets.getDisplayCutout();
        target.setPadding(left + Math.max(insets.getSystemWindowInsetLeft(), cutout == null ? 0 : cutout.getSafeInsetLeft()),
                top + Math.max(insets.getSystemWindowInsetTop(), cutout == null ? 0 : cutout.getSafeInsetTop()),
                right + Math.max(insets.getSystemWindowInsetRight(), cutout == null ? 0 : cutout.getSafeInsetRight()),
                bottom + Math.max(insets.getSystemWindowInsetBottom(), cutout == null ? 0 : cutout.getSafeInsetBottom()));
        return insets.consumeSystemWindowInsets().consumeDisplayCutout();
    }

    static Drawable backdrop() {
        return new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {0xff0d131a, 0xff16202a, 0xff221a13});
    }

    static GradientDrawable rounded(Context context, int color, float radius) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(color); shape.setCornerRadius(dp(context, radius));
        return shape;
    }

    static LinearLayout card(Context context) {
        LinearLayout card = new LinearLayout(context); card.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable shape = rounded(context, SURFACE, 18); shape.setStroke(dp(context, 1), OUTLINE);
        card.setBackground(shape);
        int padding = dp(context, 16); card.setPadding(padding, dp(context, 14), padding, dp(context, 14));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.bottomMargin = dp(context, 12); card.setLayoutParams(params);
        return card;
    }

    static TextView text(Context context, String value, float size, int color, boolean bold) {
        TextView view = new TextView(context); view.setText(value); view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        if (bold) view.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        view.setLineSpacing(0, 1.12f);
        return view;
    }

    static Button button(Context context, String value, boolean primary) {
        return button(context, value, primary, "classic");
    }

    static Button button(Context context, String value, boolean primary, String edition) {
        Button button = primary ? new PlayButton(context, edition) : new Button(context);
        button.setText(value); button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setTextColor(TEXT);
        GradientDrawable shape = rounded(context, primary ? ((PlayButton) button).fill : SURFACE_HIGH, 14);
        shape.setStroke(dp(context, primary ? 2 : 1), primary ? ((PlayButton) button).stroke : OUTLINE);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(primary ? 0x40ffffff : 0x30e7b45c), shape, null));
        button.setBackgroundTintList(null); /* Material's colorButtonNormal tint would darken the accent */
        button.setStateListAnimator(null);
        button.setMinHeight(dp(context, primary ? 54 : 46)); button.setMinimumHeight(dp(context, primary ? 54 : 46));
        int padding = dp(context, 18); button.setPadding(padding, 0, padding, 0);
        button.setGravity(Gravity.CENTER);
        return button;
    }

    static int[] playColours(String edition) {
        if ("stronghold".equals(edition)) return new int[] {0xFF183126, 0xFF9E8A50, 0xFFC3AE72, 0xFFDCF0BE};
        if ("extreme".equals(edition)) return new int[] {0xFF3A1518, 0xFFC95C3F, 0xFFF08048, 0xFFFFD09A};
        return new int[] {0xFF3C2B15, 0xFFAE8245, 0xFFE7B45C, 0xFFFFE3AA};
    }

    private static final class PlayButton extends Button {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path clip = new Path();
        private final Matrix shift = new Matrix();
        final int fill, stroke;
        private final int glow, particles;
        private final LinearGradient sheen;

        PlayButton(Context context, String edition) {
            super(context);
            int[] colours = playColours(edition);
            fill = colours[0]; stroke = colours[1];
            glow = colours[2]; particles = colours[3];
            int rgb = particles & 0x00FFFFFF;
            sheen = new LinearGradient(0, 0, dp(80), 0,
                new int[] {rgb, 0x28000000 | rgb, rgb}, null, Shader.TileMode.CLAMP);
        }

        private int dp(float value) { return Theme.dp(getContext(), value); }

        @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            super.onSizeChanged(w, h, oldw, oldh);
            clip.reset();
            clip.addRoundRect(0, 0, w, h, dp(14), dp(14), Path.Direction.CW);
        }

        @Override public void onWindowFocusChanged(boolean focused) {
            super.onWindowFocusChanged(focused);
            invalidate();
        }

        @Override protected void onWindowVisibilityChanged(int visibility) {
            super.onWindowVisibilityChanged(visibility);
            invalidate();
        }

        @Override protected void onDraw(Canvas canvas) {
            if (isEnabled()) {
                boolean motion = Build.VERSION.SDK_INT >= 26 ? ValueAnimator.areAnimatorsEnabled()
                    : Settings.Global.getFloat(getContext().getContentResolver(), Settings.Global.ANIMATOR_DURATION_SCALE, 1f) > 0;
                float phase = motion ? (SystemClock.uptimeMillis() % 3600) / 3600f : 0.25f;
                float pulse = (float) (0.5 - 0.5 * Math.cos(phase * Math.PI * 2));
                float inset = dp(1);
                int saved = canvas.save();
                canvas.clipPath(clip);
                paint.setColor(glow);
                paint.setAlpha((int) (12 + 22 * pulse));
                canvas.drawRoundRect(0, 0, getWidth(), getHeight(), dp(14), dp(14), paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(dp(2));
                paint.setAlpha((int) (45 + 85 * pulse));
                canvas.drawRoundRect(inset, inset, getWidth() - inset, getHeight() - inset, dp(13), dp(13), paint);
                paint.setStyle(Paint.Style.FILL);
                shift.setTranslate(-dp(80) + phase * (getWidth() + dp(160)), 0);
                sheen.setLocalMatrix(shift);
                paint.setShader(sheen);
                paint.setAlpha(255);
                canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
                paint.setShader(null);
                paint.setColor(particles);
                for (int i = 0; i < 6; ++i) {
                    float age = (phase + i / 6f) % 1;
                    float x = getWidth() * (0.08f + ((i * 0.173f) % 0.84f));
                    float y = getHeight() * (0.95f - age * 0.85f);
                    float radius = dp(1.5f) * (float) Math.sin(age * Math.PI);
                    paint.setAlpha((int) (190 * Math.sin(age * Math.PI)));
                    canvas.drawCircle(x, y, radius, paint);
                    if (i % 2 == 0) {
                        paint.setStrokeWidth(dp(1));
                        canvas.drawLine(x - radius * 2, y, x + radius * 2, y, paint);
                        canvas.drawLine(x, y - radius * 2, x, y + radius * 2, paint);
                    }
                }
                canvas.restoreToCount(saved);
                if (motion && hasWindowFocus() && getWindowVisibility() == View.VISIBLE && isShown()) postInvalidateDelayed(33);
            }
            super.onDraw(canvas);
        }
    }

    static void enable(View view, boolean enabled) { view.setEnabled(enabled); view.setAlpha(enabled ? 1 : .42f); }

    static EditText field(Context context, String value, String description) {
        EditText input = new EditText(context); input.setSingleLine(true); input.setText(value);
        input.setTextColor(TEXT); input.setHintTextColor(MUTED); input.setContentDescription(description);
        input.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        GradientDrawable shape = rounded(context, 0xff111920, 12); shape.setStroke(dp(context, 1), OUTLINE);
        input.setBackground(shape);
        int padding = dp(context, 12); input.setPadding(padding, dp(context, 10), padding, dp(context, 10));
        return input;
    }

    static LinearLayout row(Context context) {
        LinearLayout row = new LinearLayout(context); row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    static LinearLayout.LayoutParams weight(Context context, float weight, int startMargin) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, -2, weight);
        params.setMarginStart(dp(context, startMargin));
        return params;
    }
}
