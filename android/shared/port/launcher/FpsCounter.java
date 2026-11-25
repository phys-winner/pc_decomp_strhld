package port.launcher;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** The launcher's FPS counter (option fps_counter): the numbers of DXVK 2.4.1's HUD items "fps" and
 *  "frametimes" (src/dxvk/hud/dxvk_hud_item.cpp, HudFpsItem and HudFrameTimeItem), which FpsOverlay draws
 *  over the game, and formatting for its system lines. */
final class FpsCounter {
    /** HudFrameTimeItem::NumDataPoints: frames in the graph, one HUD unit wide each. */
    static final int POINTS = 304;
    /** HudFpsItem::UpdateInterval: the frame rate is recomputed every half second. */
    static final long UPDATE_US = 500_000;
    /** The HUD's colours: (1, 0.25, 0.25) and white as DXVK writes them to an sRGB swapchain. */
    static final int LABEL = 0xFFFF8989, VALUE = 0xFFFFFFFF;

    private static final float TARGET_US = 16_666.6f;
    private static final String[] THERMAL = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    private static final String[] GAME_MODE = {"", "standard", "performance", "battery", "custom"};

    private FpsCounter() {}

    /** The overlay's scale for a display density: DXVK's HUD at scale 2 on a 420 dpi phone, the same
     *  physical size elsewhere; quarter steps between 1 and 4. */
    static float hudScale(float density) {
        float scale = Math.round(density * 0.76f * 4.0f) / 4.0f;
        return Math.max(1.0f, Math.min(4.0f, scale));
    }

    /** HudFpsItem::update: the frames since the last update over the time since, to a tenth. */
    static String frameRate(long frames, long elapsedUs) {
        long tenths = elapsedUs > 0 && frames > 0 ? 10_000_000L * frames / elapsedUs : 0;
        return (tenths / 10) + "." + (tenths % 10);
    }

    static float fpsHeight(int intervalUs, int scaleFps) {
        return intervalUs > 0 ? Math.min(1f, 1_000_000f / intervalUs / scaleFps) : 0f;
    }

    /** ... and its colour: green up to 16.7 ms, through yellow to red at 66.7 ms. The unorm colour is
     *  linear in DXVK's HUD shader, which encodes it for the sRGB swapchain. */
    static int color(float us) {
        float r = Math.min(Math.max(-1.0f + us / TARGET_US, 0.0f), 1.0f);
        float g = Math.min(Math.max(3.0f - us / TARGET_US, 0.0f), 1.0f);
        float l = (float) Math.sqrt(r * r + g * g);
        int red = (int) (255.0f * (r / l)), green = (int) (255.0f * (g / l));
        return 0xFF000000 | (srgb(red) << 16) | (srgb(green) << 8);
    }

    /** min and max of the graph's frames as DXVK prints them: milliseconds to a tenth, truncated. */
    static String milliseconds(long tenthsOfMs) {
        return (tenthsOfMs / 10) + "." + (tenthsOfMs % 10);
    }

    /** A frame time in tenths of a millisecond, as HudFrameTimeItem::render counts min and max. */
    static long tenths(int us) {
        return (long) (us / 100.0f);
    }

    /** encodeSrgb of hud_frag_common.glsl for an 8-bit channel. */
    static int srgb(int unorm) {
        double linear = unorm / 255.0;
        double encoded = linear <= 0.0031308 ? linear * 12.92 : Math.pow(linear, 5.0 / 12.0) * 1.055 - 0.055;
        return (int) Math.round(Math.min(Math.max(encoded, 0.0), 1.0) * 255.0);
    }

    /* ------------------------------------------------------------------ system lines */

    /** A value cut to max characters with an ellipsis. */
    static String shorten(String value, int max) {
        return value.length() <= max ? value : value.substring(0, Math.max(0, max - 1)) + "…";
    }

    /** "0.73 · 0.92 · 2.34 GHz": the clusters' current clocks. */
    static String clocks(long[] khz) {
        if (khz.length == 0) return "";
        StringBuilder text = new StringBuilder();
        for (long k : khz) {
            if (text.length() > 0) text.append(" · ");
            text.append(String.format(Locale.ROOT, "%.2f", k / 1e6));
        }
        return text.append(" GHz").toString();
    }

    /** utime + stime of a /proc/<pid>[/task/<tid>]/stat line (fields 14 and 15), -1 if unreadable. The
     *  thread's name (field 2) may hold spaces and parentheses: fields count from its last ')'. */
    static long statTicks(String stat) {
        int close = stat.lastIndexOf(')');
        if (close < 0) return -1;
        String[] fields = stat.substring(close + 1).trim().split("\\s+");
        if (fields.length < 13) return -1;
        try {
            return Long.parseLong(fields[11]) + Long.parseLong(fields[12]);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    static String statName(String stat) {
        int open = stat.indexOf('('), close = stat.lastIndexOf(')');
        return open >= 0 && close > open ? stat.substring(open + 1, close) : "?";
    }

    /** "game 1.9 cores · Thread-5 98%, d3d9-render 66%, AudioTrack 24%": busy = {name, percent}. */
    static String load(double cores, List<String[]> busy) {
        List<String[]> sorted = new ArrayList<>(busy);
        sorted.sort((a, b) -> Double.compare(Double.parseDouble(b[1]), Double.parseDouble(a[1])));
        StringBuilder text = new StringBuilder(String.format(Locale.ROOT, "game %.1f cores", cores));
        for (int i = 0; i < Math.min(3, sorted.size()); ++i) {
            long percent = Math.round(Double.parseDouble(sorted.get(i)[1]));
            if (percent < 1) break;
            text.append(i == 0 ? " · " : ", ").append(sorted.get(i)[0]).append(' ').append(percent).append('%');
        }
        return text.toString();
    }

    /** /proc/meminfo in bytes. */
    static Map<String, Long> meminfo(String text) {
        Map<String, Long> result = new HashMap<>();
        for (String line : text.split("\n")) {
            String[] parts = line.trim().split("[:\\s]+");
            if (parts.length >= 2) {
                try { result.put(parts[0], Long.parseLong(parts[1]) * 1024L); } catch (NumberFormatException ignored) { }
            }
        }
        return result;
    }

    /** "game 2.31 GB · free 1.87 / 11.27 GB". */
    static String memory(long gameBytes, long availableBytes, long totalBytes) {
        final double gb = 1024.0 * 1024.0 * 1024.0;
        return String.format(Locale.ROOT, "game %.2f GB · free %.2f / %.2f GB", gameBytes / gb, availableBytes / gb, totalBytes / gb);
    }

    /** "severe · 104% of the limit · mode standard": PowerManager's thermal status, its headroom
     *  (1.0 = the severe throttling threshold) and GameManager's mode, where Android gives them. */
    static String thermal(int status, float headroom, int gameMode) {
        StringBuilder text = new StringBuilder(status >= 0 && status < THERMAL.length ? THERMAL[status] : "unknown");
        if (!Float.isNaN(headroom) && headroom >= 0) text.append(String.format(Locale.ROOT, " · %d%% of the limit", Math.round(headroom * 100)));
        if (gameMode > 0 && gameMode < GAME_MODE.length) text.append(" · mode ").append(GAME_MODE[gameMode]);
        return text.toString();
    }

    static String battery(int level, int scale, int tenthsC) {
        List<String> parts = new ArrayList<>();
        if (level >= 0 && scale > 0 && level <= scale) parts.add(Math.round(100.0f * level / scale) + "%");
        if (tenthsC != Integer.MIN_VALUE) parts.add(String.format(Locale.ROOT, "%.1f °C", tenthsC / 10.0));
        return String.join(" · ", parts);
    }

    /** Instantaneous net power at the battery, from microamperes and millivolts. */
    static String power(long microAmps, int milliVolts) {
        if (microAmps == Long.MIN_VALUE || milliVolts <= 0) return "";
        double watts = Math.abs((double) microAmps) * milliVolts / 1_000_000_000.0;
        String direction = microAmps < 0 ? "battery draw" : microAmps > 0 ? "charging" : "battery idle";
        return String.format(Locale.ROOT, "%.1f W · %s", watts, direction);
    }

    static String screen(int width, int height, float refresh, double fpsLimit) {
        StringBuilder text = new StringBuilder(width + "x" + height);
        if (refresh > 0) text.append(" · ").append(Math.round(refresh)).append(" Hz");
        if (Double.isFinite(fpsLimit) && fpsLimit > 0) {
            String limit = String.format(Locale.ROOT, "%.2f", fpsLimit).replaceFirst("\\.?0+$", "");
            text.append(" · limit ").append(limit).append(" FPS");
        }
        return text.toString();
    }

    /** CPUs in a sysfs range list such as "0-7" or "0-3,6". */
    static int cpuRangeCount(String ranges) {
        int count = 0;
        for (String part : ranges.split(",")) {
            String[] bounds = part.trim().split("-");
            try {
                if (bounds.length == 1) count += Integer.parseInt(bounds[0]) >= 0 ? 1 : 0;
                else if (bounds.length == 2) count += Integer.parseInt(bounds[1]) - Integer.parseInt(bounds[0]) + 1;
                else return 0;
            } catch (NumberFormatException e) {
                return 0;
            }
        }
        return count;
    }

}
