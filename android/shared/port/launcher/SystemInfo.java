package port.launcher;

import android.app.ActivityManager;
import android.app.GameManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.display.DisplayManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import android.util.DisplayMetrics;
import android.view.Display;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.DoubleSupplier;
import java.util.function.Supplier;
import java.util.regex.Pattern;

/** System rows for the FPS counter. All device and process sampling runs off the UI thread. */
final class SystemInfo {
    private static final Pattern NON_DIE = Pattern.compile("batt|charger|usb|skin|pmic|ntc|ocp|vdroop|bcl|modem|disp");
    private static final Pattern CPU = Pattern.compile("cpu|s5p-tmu");
    private static final Pattern GPU = Pattern.compile("gpu|g3d|kgsl|mali");

    private final Context context;
    private final int mask;
    private final IntentFilter batteryChanged = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
    private final Supplier<String> renderer, gpuStrings;
    private final DoubleSupplier fpsLimit;
    private final long clockTick = Math.max(1, Os.sysconf(OsConstants._SC_CLK_TCK));
    private final long page = Math.max(1, Os.sysconf(OsConstants._SC_PAGESIZE));
    private final Map<String, Long> threadTicks = new HashMap<>();
    private final List<File> cpuSensors = new ArrayList<>(), gpuSensors = new ArrayList<>();
    private Map<String, String> probe;
    private String soc;
    private long processTicks = -1, sampledNs, nextHeadroomMs;
    private float headroom = Float.NaN;

    SystemInfo(Context context, int mask, Supplier<String> renderer, DoubleSupplier fpsLimit, Supplier<String> gpuStrings) {
        this.context = context.getApplicationContext();
        this.mask = mask;
        this.renderer = renderer;
        this.fpsLimit = fpsLimit;
        this.gpuStrings = gpuStrings;
    }

    synchronized String[][] sample() {
        if (soc == null) {
            probe = Probe.parse(Probe.read(new File(State.folder(context), Probe.REPORT)));
            String model = Build.VERSION.SDK_INT >= 31
                ? (Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL).trim() : Build.HARDWARE;
            if (HudField.CPU_TEMP.in(mask) || HudField.GPU_TEMP.in(mask))
                findSensors(new File("/sys/class/thermal"), cpuSensors, gpuSensors);
            if (HudField.GPU_TEMP.in(mask)) for (String path : Arrays.asList("/sys/class/kgsl/kgsl-3d0/temp", "/sys/class/misc/mali0/device/temp")) {
                File sensor = new File(path);
                if (sensor.canRead()) gpuSensors.add(sensor);
            }
            soc = model + " · " + cpuCount() + " cores";
        }

        List<String[]> rows = new ArrayList<>();
        if (HudField.CPU.in(mask)) add(rows, "CPU:", soc);
        if (HudField.CLOCK.in(mask)) add(rows, "Clock:", FpsCounter.clocks(clusterKHz()));
        if (HudField.LOAD.in(mask)) add(rows, "Load:", load());
        if (HudField.CPU_TEMP.in(mask)) add(rows, "CPU temp:", temperature(cpuSensors));
        if (HudField.GPU.in(mask) || HudField.DRIVER.in(mask) || HudField.API.in(mask)) renderInfo(rows);
        if (HudField.GPU_TEMP.in(mask)) add(rows, "GPU temp:", temperature(gpuSensors));
        if (HudField.RAM.in(mask)) add(rows, "RAM:", memory());
        if (HudField.THERMAL.in(mask)) add(rows, "Thermal:", thermal());
        if (HudField.BATTERY.in(mask) || HudField.POWER.in(mask)) battery(rows);
        if (HudField.SCREEN.in(mask)) add(rows, "Screen:", screen());
        return rows.toArray(new String[0][]);
    }

    private static void add(List<String[]> rows, String label, String value) {
        if (!value.isEmpty()) rows.add(new String[]{label, value});
    }

    private void renderInfo(List<String[]> rows) {
        Spec spec = Spec.get(context);
        String id = renderer.get();
        Spec.Mode mode = spec.mode(id != null ? id : spec.defaultMode);
        String kind = mode != null ? mode.kind : "";
        String prefix = "panvk".equals(kind) || "turnip".equals(kind) ? kind + "." : "stock.";
        boolean vulkan = "vulkan".equals(kind) || "turnip".equals(kind) || "panvk".equals(kind);
        String device = vulkan ? probe.getOrDefault(prefix + "device", "") : "";
        String driver = vulkan ? probe.getOrDefault(prefix + "driver", "") : "";
        String nativeGpu = gpuStrings != null ? gpuStrings.get() : null;
        if (nativeGpu != null) {
            String[] reported = nativeGpu.split("\n", 3);
            if (!reported[0].isEmpty()) device = reported[0];
            if (reported.length > 1 && !reported[1].isEmpty()) driver = reported[1];
        }
        if ("turnip".equals(kind) || "panvk".equals(kind)) {
            String own = Os.getenv("turnip".equals(kind) ? "PORT_TURNIP_DRIVER" : "PORT_PANVK_DRIVER");
            if (own != null && !own.isEmpty())
                driver = new File(own).getName() + (driver.isEmpty() ? "" : " · " + driver);
        }
        if (HudField.GPU.in(mask)) add(rows, "GPU:", FpsCounter.shorten(device, 44));
        if (HudField.DRIVER.in(mask)) add(rows, "Driver:", FpsCounter.shorten(driver, 44));
        if (mode != null && HudField.API.in(mask)) {
            String version = vulkan ? probe.getOrDefault(prefix + "api", "") : "";
            add(rows, "API:", mode.title + (version.isEmpty() ? "" : " · Vulkan " + version));
        }
    }

    private String screen() {
        DisplayManager displays = context.getSystemService(DisplayManager.class);
        Display display = displays != null ? displays.getDisplay(Display.DEFAULT_DISPLAY) : null;
        DisplayMetrics metrics = context.getResources().getDisplayMetrics();
        if (display != null) {
            metrics = new DisplayMetrics();
            display.getRealMetrics(metrics);
        }
        return FpsCounter.screen(metrics.widthPixels, metrics.heightPixels,
            display != null ? display.getRefreshRate() : 0, fpsLimit.getAsDouble());
    }

    static void findSensors(File directory, List<File> cpu, List<File> gpu) {
        File[] zones = directory.listFiles((dir, name) -> name.startsWith("thermal_zone"));
        if (zones == null) return;
        for (File zone : zones) {
            String type = read(new File(zone, "type")).trim().toLowerCase(Locale.ROOT);
            if (NON_DIE.matcher(type).find()) continue;
            File sensor = new File(zone, "temp");
            if (!sensor.canRead()) continue;
            if (GPU.matcher(type).find()) gpu.add(sensor);
            else if (CPU.matcher(type).find()) cpu.add(sensor);
        }
    }

    static String temperature(List<File> sensors) {
        double hottest = 0;
        for (File sensor : sensors) {
            try {
                int raw = Integer.parseInt(read(sensor).trim());
                double c = raw > 1500 ? raw / 1000.0 : raw > 150 ? raw / 10.0 : raw;
                if (c >= 1 && c <= 150) hottest = Math.max(hottest, c);
            } catch (NumberFormatException ignored) { }
        }
        return hottest > 0 ? String.format(Locale.ROOT, "%.1f °C", hottest) : "";
    }

    /** The game's CPU time over the last second in cores, and its three busiest threads. */
    private String load() {
        long now = System.nanoTime();
        String self = read(new File("/proc/self/stat"));
        long ticks = FpsCounter.statTicks(self);
        Map<String, Long> threads = new HashMap<>();
        File[] tasks = new File("/proc/self/task").listFiles();
        if (tasks != null) for (File task : tasks) {
            String stat = read(new File(task, "stat"));
            long t = FpsCounter.statTicks(stat);
            if (t >= 0) threads.put(task.getName() + "\t" + FpsCounter.statName(stat), t);
        }
        String result = "";
        if (processTicks >= 0 && ticks >= processTicks && now > sampledNs) {
            double seconds = (now - sampledNs) / 1e9;
            List<String[]> busy = new ArrayList<>();
            for (Map.Entry<String, Long> e : threads.entrySet()) {
                Long before = threadTicks.get(e.getKey());
                if (before == null || e.getValue() < before) continue;
                double percent = 100.0 * (e.getValue() - before) / clockTick / seconds;
                busy.add(new String[]{e.getKey().substring(e.getKey().indexOf('\t') + 1), Double.toString(percent)});
            }
            result = FpsCounter.load((ticks - processTicks) / (double) clockTick / seconds, busy);
        }
        processTicks = ticks;
        sampledNs = now;
        threadTicks.clear();
        threadTicks.putAll(threads);
        return result;
    }

    private String memory() {
        String statm = read(new File("/proc/self/statm"));
        long resident = 0;
        String[] fields = statm.trim().split("\\s+");
        if (fields.length > 1) {
            try { resident = Long.parseLong(fields[1]) * page; } catch (NumberFormatException ignored) { }
        }
        Map<String, Long> info = FpsCounter.meminfo(read(new File("/proc/meminfo")));
        long total = info.getOrDefault("MemTotal", 0L), available = info.getOrDefault("MemAvailable", -1L);
        if (total == 0 || available < 0) {
            ActivityManager.MemoryInfo m = new ActivityManager.MemoryInfo();
            ((ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(m);
            total = m.totalMem;
            available = m.availMem;
        }
        return FpsCounter.memory(resident, available, total);
    }

    private String thermal() {
        PowerManager power = context.getSystemService(PowerManager.class);
        if (power == null || Build.VERSION.SDK_INT < 29) return "";
        int status = power.getCurrentThermalStatus();
        long now = SystemClock.elapsedRealtime();
        if (Build.VERSION.SDK_INT >= 30 && now >= nextHeadroomMs) {
            headroom = power.getThermalHeadroom(10);
            nextHeadroomMs = now + 10_000;
        }
        int mode = 0;
        if (Build.VERSION.SDK_INT >= 31) {
            GameManager games = context.getSystemService(GameManager.class);
            if (games != null) mode = games.getGameMode();
        }
        return FpsCounter.thermal(status, headroom, mode);
    }

    private void battery(List<String[]> rows) {
        Intent sticky = context.registerReceiver(null, batteryChanged);
        if (sticky == null || !sticky.getBooleanExtra(BatteryManager.EXTRA_PRESENT, true)) return;
        if (HudField.BATTERY.in(mask)) add(rows, "Battery:", FpsCounter.battery(sticky.getIntExtra(BatteryManager.EXTRA_LEVEL, -1),
            sticky.getIntExtra(BatteryManager.EXTRA_SCALE, -1),
            sticky.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE)));
        BatteryManager manager = context.getSystemService(BatteryManager.class);
        if (HudField.POWER.in(mask) && manager != null) {
            long current = manager.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
            add(rows, "Power:", FpsCounter.power(current, sticky.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0)));
        }
    }

    /** The current clock of every cpufreq policy (a cluster), in policy order. */
    private static long[] clusterKHz() {
        File[] policies = new File("/sys/devices/system/cpu/cpufreq").listFiles((dir, name) -> name.matches("policy\\d+"));
        if (policies == null) return new long[0];
        Arrays.sort(policies, (a, b) -> Integer.compare(Integer.parseInt(a.getName().substring(6)), Integer.parseInt(b.getName().substring(6))));
        long[] khz = new long[policies.length];
        int n = 0;
        for (File policy : policies) {
            try { khz[n] = Long.parseLong(read(new File(policy, "scaling_cur_freq")).trim()); n++; } catch (NumberFormatException ignored) { }
        }
        return Arrays.copyOf(khz, n);
    }

    private static int cpuCount() {
        String possible = read(new File("/sys/devices/system/cpu/possible")).trim();
        int count = FpsCounter.cpuRangeCount(possible);
        return count > 0 ? count : Runtime.getRuntime().availableProcessors();
    }

    private static String read(File file) {
        try {
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
        } catch (IOException | RuntimeException e) {
            return "";
        }
    }
}
