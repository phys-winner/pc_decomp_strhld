package port.launcher;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.util.function.Supplier;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** The door between Stronghold's own launcher shell (org.stronghold.port) and the classes it takes
 *  unchanged from the shared launcher base (android/shared/port/launcher): the FPS overlay with its
 *  per-row settings, the game-mode tip and the MD5 check of a checksum-named archive. The settings
 *  use the base's own keys in files/launcher/choices.properties, which every game process reads. */
public final class Shared {
    private static final String FPS = "option." + Spec.FPS_COUNTER;
    private static final Pattern GLES = Pattern.compile("OpenGL ES (\\d+\\.\\d+)");

    private Shared() { }

    /** The base's text column for a language tag list such as "ru-RU,en-US": en, ru, de, pl. */
    public static int column(String languageTags) {
        String language = languageTags == null ? "" : languageTags.split(",")[0].trim().split("[-_]")[0];
        if (language.isEmpty()) language = java.util.Locale.getDefault().getLanguage();
        return "ru".equals(language) || "uk".equals(language) || "be".equals(language) ? 1
            : "de".equals(language) ? 2 : "pl".equals(language) ? 3 : 0;
    }

    /* ------------------------------------------------------------ FPS overlay */

    public static boolean hudEnabled(Context context) {
        return !"0".equals(State.get(context, State.CHOICES, FPS, "0"));
    }

    /** The master switch. The rows chosen before stay; the first enable shows the frame rate only. */
    public static void hudEnabled(Context context, boolean on) {
        State.update(context, State.CHOICES, prefs -> {
            prefs.setProperty(HudField.KEY, Integer.toString(HudField.mask(prefs, prefs.getProperty(FPS, "0"))));
            prefs.setProperty(FPS, on ? "1" : "0");
        });
    }

    public static String[] hudRows(int column) {
        HudField[] fields = HudField.values();
        String[] names = new String[fields.length];
        for (HudField field : fields) names[field.ordinal()] = field.title[column >= 0 && column < field.title.length ? column : 0];
        return names;
    }

    public static int hudMask(Context context) {
        return HudField.read(context, State.get(context, State.CHOICES, FPS, "0"));
    }

    public static void hudMask(Context context, int mask) {
        State.put(context, State.CHOICES, HudField.KEY, Integer.toString(mask & HudField.ALL));
    }

    /** The overlay for a game activity, or null while the player keeps the counter off: nothing is
     *  created and the native side records no frame times. The GPU supplier returns GL_RENDERER and
     *  GL_VERSION of the game's context separated by a newline (null before the context exists). */
    public static FpsOverlay overlay(Context context, FpsOverlay.Source frames, Supplier<String> gpu) {
        Supplier<String> strings = () -> {
            String value = gpu.get();
            if (value != null) {
                Matcher version = GLES.matcher(value);
                if (version.find()) Spec.api = "OpenGL ES " + version.group(1);
            }
            return value;
        };
        return FpsOverlay.create(context, State.get(context, State.CHOICES, FPS, "0"), frames, () -> "gles", () -> 0, strings);
    }

    /* ---------------------------------------------------------- game-mode tip */

    public static boolean tipVendor() { return HyperOs.vendor() != HyperOs.NONE; }

    /** The tip is still to be offered in this process (HyperOs.due). */
    public static boolean tipDue(Context context) { return HyperOs.due(context, Spec.get(context)); }

    public static void tipShown() { HyperOs.shown(); }

    public static String tipSystem() { return HyperOs.system(HyperOs.vendor(), Build.MANUFACTURER); }

    /** The tool the "Open …" button names; null when the tip has no such button. */
    public static String tipTool(int column) { return HyperOs.tool(HyperOs.vendor(), column); }

    public static String tipAdvice(int column) { return HyperOs.advice(HyperOs.vendor(), column); }

    public static boolean tipOpen(Activity activity) { return HyperOs.open(activity); }

    public static boolean tipNever(Context context) { return HyperOs.never(context); }

    public static void tipNever(Context context, boolean never) { HyperOs.never(context, never); }

    /* ------------------------------------------------------------ archive MD5 */

    public interface Md5Progress { void update(long done, long total); }

    /** The MD5 an archive's file name carries, or null. */
    public static String md5Expected(String name) { return ArchiveMd5.expected(name); }

    /** Reads the whole file by position (its own position stays where it was); interrupt the thread to cancel. */
    public static boolean md5Matches(FileChannel file, String expected, Md5Progress progress) throws IOException {
        return ArchiveMd5.matches(file, expected, progress::update);
    }
}
