package org.stronghold.port;

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
        Button button = new Button(context); button.setText(value); button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setTextColor(primary ? ON_ACCENT : TEXT);
        GradientDrawable shape = rounded(context, primary ? ACCENT : SURFACE_HIGH, 14);
        if (!primary) shape.setStroke(dp(context, 1), OUTLINE);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(primary ? 0x40ffffff : 0x30e7b45c), shape, null));
        button.setBackgroundTintList(null); /* Material's colorButtonNormal tint would darken the accent */
        button.setStateListAnimator(null);
        button.setMinHeight(dp(context, 46)); button.setMinimumHeight(dp(context, 46));
        int padding = dp(context, 18); button.setPadding(padding, 0, padding, 0);
        button.setGravity(Gravity.CENTER);
        return button;
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
