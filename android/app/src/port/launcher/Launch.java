package port.launcher;

import android.content.Context;

/** Stand-in for the launcher base's Launch: only what the shared FpsOverlay refers to. */
final class Launch {
    static final String EXTRA_RENDERER = "renderer";

    private Launch() { }

    static String option(Context context, Spec.Option option) {
        return State.get(context, State.CHOICES, "option." + option.key, option.value);
    }
}
