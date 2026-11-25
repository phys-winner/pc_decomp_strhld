package org.stronghold.port;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import android.os.Environment;
import android.os.Process;
import android.util.AtomicFile;
import android.util.Log;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.time.Instant;

/** Session log rotation from Heroes V and the native journal and exit report from UT3. */
final class CrashMonitor {
    static final String OPTION = "monitorCrashes";
    private static final int LOG_LIMIT = 64 * 1024, TOMBSTONE_LIMIT = 2 * 1024 * 1024;
    private static final String NATIVE = "stronghold-crash.txt", JAVA = "stronghold-java-crash.txt";
    private static final String REPORT = "last-crash.txt", TOMBSTONE = "last-crash.tombstone.pb";
    /** Written by the native side at the first frame the display accepted (crash_journal.c). */
    private static final String FIRST_FRAME = "stronghold-first-frame";
    /** The last critical memory signal of the session: "<time ms> <trim level>". */
    private static final String MEMORY = "stronghold-memory.txt";
    /** Causes in the report's "Cause:" line; the launcher explains each in the player's language. */
    static final String JAVA_FAILURE = "java", NATIVE_SIGNAL = "native-signal", ANR = "anr", LOW_MEMORY = "low-memory",
            LOW_MEMORY_SUSPECTED = "low-memory-suspected", EXIT_CODE = "exit", SYSTEM_KILL = "system-kill", UNKNOWN = "unknown";
    /** The shared launcher's renderer failure policy (owner's decision of 24 November 2026): OpenGL ES did not start
     *  (before the first frame) or lost its context (before the first frame or within RENDERER_WINDOW_MS after it);
     *  RENDERER_LATE: a renderer failure outside these rules, an ordinary crash. The record is the base's
     *  (native/replacements/renderer_failure.h, android/shared/native), written by renderer_present.c. */
    static final String RENDERER_START = "renderer-start", RENDERER_LOST = "renderer-lost", RENDERER_LATE = "renderer-late";
    /** The renderer id of the base's list ("gles"): the record names it, the QA switch matches it. */
    static final String RENDERER_ID = "gles";
    private static final String RENDERER_FAILURE = "stronghold-renderer-failure.txt";
    private static final long RENDERER_WINDOW_MS = 30000, RENDERER_SLACK_MS = 2000;

    /** What the launcher shows at its next start. */
    static final class Summary {
        File report;
        String game = "", cause = UNKNOWN, detail = "";
        /** False for a report of an earlier build, which names no cause. */
        boolean presented, described;
    }

    private CrashMonitor() { }

    private static File directory(String edition) {
        return new File(new File(Environment.getExternalStorageDirectory(), "Stronghold"), GameFiles.userdata(edition));
    }

    private static File session(Context context, String edition) {
        return new File(context.getFilesDir(), "crash-" + edition + ".session");
    }

    static synchronized boolean begin(Context context, String edition) {
        File data = directory(edition), state = session(context, edition);
        try (RandomAccessFile lock = lock(state); FileLock held = lock.getChannel().lock()) {
            if (collect(context, edition, data, state, true)) return true;
            File log = new File(data, "stronghold.log");
            if (log.isFile()) Files.move(log.toPath(), new File(data, "stronghold.previous.log").toPath(),
                                        StandardCopyOption.REPLACE_EXISTING);
            Files.deleteIfExists(new File(data, NATIVE).toPath());
            Files.deleteIfExists(new File(data, JAVA).toPath());
            Files.deleteIfExists(new File(data, FIRST_FRAME).toPath());
            Files.deleteIfExists(new File(data, MEMORY).toPath());
            Files.deleteIfExists(new File(data, RENDERER_FAILURE).toPath());
            AtomicFile file = new AtomicFile(state);
            FileOutputStream stream = file.startWrite();
            try {
                DataOutputStream output = new DataOutputStream(stream);
                output.writeInt(Process.myPid());
                output.writeLong(System.currentTimeMillis());
                output.writeUTF(Diagnostics.version(context));
                output.flush();
                file.finishWrite(stream);
            } catch (IOException error) {
                file.failWrite(stream);
                throw error;
            }
            Thread.UncaughtExceptionHandler fallback = Thread.getDefaultUncaughtExceptionHandler();
            Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
                try { recordLaunchFailure(edition, Log.getStackTraceString(error)); }
                finally {
                    if (fallback != null) fallback.uncaughtException(thread, error);
                    else Process.killProcess(Process.myPid());
                }
            });
            return true;
        } catch (IOException | RuntimeException error) {
            Log.w("Stronghold", "Cannot start crash monitor", error);
            return false;
        }
    }

    private static RandomAccessFile lock(File state) throws IOException {
        return new RandomAccessFile(new File(state.getParentFile(), state.getName() + ".lock"), "rw");
    }

    /** Where renderer_present.c writes the renderer failure record of this edition (PORT_RENDERER_FAILURE). */
    static File rendererFailureFile(String edition) { return new File(directory(edition), RENDERER_FAILURE); }

    /** The record of the session that started at `started` (process pid), parsed as the base's RendererFailure
     *  reads it: {kind, since_present_ms, reason}; null when there is none. */
    static String[] rendererFailure(File data, int pid, long started) {
        File file = new File(data, RENDERER_FAILURE);
        if (!file.isFile() || file.length() > 4096 || file.lastModified() + RENDERER_SLACK_MS < started) return null;
        java.util.Properties values = new java.util.Properties();
        try {
            String[] lines = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).split("\n");
            if (lines.length < 2 || !"port-renderer-failure 1".equals(lines[0].trim())) return null;
            for (String line : lines) {
                int at = line.indexOf('=');
                if (at > 0) values.setProperty(line.substring(0, at), line.substring(at + 1).replace("\r", ""));
            }
        } catch (IOException | RuntimeException error) { return null; }
        String kind = values.getProperty("kind", "");
        if (!"start".equals(kind) && !"lost".equals(kind)) return null;
        try {
            long recordPid = Long.parseLong(values.getProperty("pid", "0").trim());
            if (pid > 0 && recordPid > 0 && recordPid != pid) return null;
        } catch (NumberFormatException invalid) { return null; }
        return new String[] {kind, values.getProperty("since_present_ms", "-1").trim(), values.getProperty("reason", "").trim()};
    }

    /** The base's rules (RendererFailure.inRules): "start" before the first frame, "lost" before it or within the window. */
    static String rendererCause(String[] failure) {
        long since;
        try { since = Long.parseLong(failure[1]); } catch (NumberFormatException invalid) { since = -1; }
        if ("lost".equals(failure[0])) return since < 0 || since <= RENDERER_WINDOW_MS ? RENDERER_LOST : RENDERER_LATE;
        return since < 0 ? RENDERER_START : RENDERER_LATE;
    }

    static void recordLaunchFailure(String edition, String text) {
        try { write(new File(directory(edition), JAVA), text.substring(0, Math.min(text.length(), LOG_LIMIT))); }
        catch (IOException | RuntimeException error) { Log.w("Stronghold", "Cannot record Java failure", error); }
    }

    /** The system's last warnings before it kills for memory; kept for Android without exit reasons (API < 30). */
    static void memoryPressure(String edition, int level) {
        try { write(new File(directory(edition), MEMORY), System.currentTimeMillis() + " " + level); }
        catch (IOException | RuntimeException error) { Log.w("Stronghold", "Cannot record memory pressure", error); }
    }

    /** The head of a report written by collect(). */
    static Summary summary(File report) {
        Summary summary = new Summary();
        summary.report = report;
        try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(
                new java.io.FileInputStream(report), StandardCharsets.UTF_8))) {
            String line;
            for (int n = 0; n < 12 && (line = reader.readLine()) != null && !line.isEmpty(); n++) {
                if (line.startsWith("Game: ")) summary.game = line.substring(6);
                else if (line.startsWith("Cause: ")) { summary.cause = line.substring(7).trim(); summary.described = true; }
                else if (line.startsWith("Detail: ")) summary.detail = line.substring(8).trim();
                else if (line.startsWith("First frame presented: ")) summary.presented = line.endsWith("yes");
            }
        } catch (IOException | RuntimeException error) { Log.w("Stronghold", "Cannot read crash report", error); }
        return summary;
    }

    private static String signalNumber(String journal) {
        java.util.regex.Matcher signal = java.util.regex.Pattern.compile("Native signal: 0x([0-9a-f]+)").matcher(journal);
        try { return signal.find() ? Long.toString(Long.parseLong(signal.group(1), 16)) : ""; }
        catch (NumberFormatException invalid) { return ""; }
    }

    /** A critical memory warning within 15 s before the last line of the game log. */
    private static boolean memoryKillLikely(File data) throws IOException {
        String[] record = tail(new File(data, MEMORY), 64).trim().split(" ");
        File log = new File(data, "stronghold.log");
        try { return record.length == 2 && log.isFile() && Math.abs(log.lastModified() - Long.parseLong(record[0])) < 15000; }
        catch (NumberFormatException invalid) { return false; }
    }

    static synchronized void end(Context context, String edition) {
        File state = session(context, edition);
        if (!state.exists()) return;
        try (RandomAccessFile lock = lock(state); FileLock held = lock.getChannel().lock();
             DataInputStream input = new DataInputStream(new AtomicFile(state).openRead())) {
            if (input.readInt() == Process.myPid()) new AtomicFile(state).delete();
        } catch (IOException | RuntimeException error) { Log.w("Stronghold", "Cannot close crash session", error); }
    }

    static synchronized File latest(Context context) {
        File newest = null;
        for (String edition : GameFiles.EDITIONS) {
            File state = session(context, edition), data = directory(edition);
            if (state.exists()) try (RandomAccessFile lock = lock(state); FileLock held = lock.getChannel().lock()) {
                collect(context, edition, data, state, false);
            } catch (IOException | RuntimeException error) { Log.w("Stronghold", "Cannot collect crash report", error); }
            File report = new File(data, REPORT);
            if (report.isFile() && (newest == null || report.lastModified() > newest.lastModified())) newest = report;
        }
        return newest;
    }

    /** Returns true while the recorded owner is still running. */
    private static boolean collect(Context context, String edition, File data, File state, boolean replacing) throws IOException {
        if (!state.exists()) return false;
        int pid;
        long started;
        String version;
        try (DataInputStream input = new DataInputStream(new AtomicFile(state).openRead())) {
            pid = input.readInt();
            started = input.readLong();
            version = input.readUTF();
        }
        String process = context.getPackageName() + ":" + edition;
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        if (pid == Process.myPid()) return true;
        /* A renderer failure record: the game is ending (its process is killed right after writing it). */
        String[] renderer = rendererFailure(data, pid, started);
        java.util.List<ActivityManager.RunningAppProcessInfo> running = manager == null ? null : manager.getRunningAppProcesses();
        if (running != null && renderer == null) for (ActivityManager.RunningAppProcessInfo app : running)
            if (app.pid == pid && process.equals(app.processName)) return true;
        String nativeReason = tail(new File(data, NATIVE), 4096).trim();
        String javaReason = tail(new File(data, JAVA), LOG_LIMIT).trim();
        ApplicationExitInfo exit = Build.VERSION.SDK_INT >= 30 && manager != null ?
                ExitHistory.find(manager, context.getPackageName(), process, pid, started) : null;
        if (renderer == null && javaReason.isEmpty() && (nativeReason.equals("Game exit: 0") || (nativeReason.isEmpty() &&
                exit != null && ExitHistory.intentional(exit)))) {
            new AtomicFile(state).delete();
            return false;
        }
        boolean presented = new File(data, FIRST_FRAME).isFile(), memory = memoryKillLikely(data);
        String cause, detail = "";
        /* The renderer failure comes first: the game ended itself because of it (the base's Launch.lastExit order). */
        if (renderer != null) { cause = rendererCause(renderer); detail = renderer[2]; }
        else if (!javaReason.isEmpty()) cause = JAVA_FAILURE;
        else if (nativeReason.startsWith("Native signal:")) { cause = NATIVE_SIGNAL; detail = signalNumber(nativeReason); }
        else if (exit != null) { cause = ExitHistory.cause(exit); detail = ExitHistory.detail(exit); }
        else if (nativeReason.startsWith("Game exit:")) { cause = EXIT_CODE; detail = nativeReason.substring(10).trim(); }
        else if (memory) cause = LOW_MEMORY_SUSPECTED;
        else cause = UNKNOWN;
        boolean unknown = renderer == null && nativeReason.isEmpty() && javaReason.isEmpty() && exit == null && !memory;
        if (unknown && !replacing) return false;
        if (unknown && new File(data, REPORT).isFile()) {
            new AtomicFile(state).delete();
            return false;
        }
        StringBuilder report = new StringBuilder("Stronghold crash report\n")
                .append("Game: ").append(GameFiles.title(edition)).append("\nVersion: ").append(version)
                .append("\nSession started: ").append(Instant.ofEpochMilli(started))
                .append("\nDevice: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append("\nAndroid: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")")
                .append("\nCause: ").append(cause).append("\nDetail: ").append(detail)
                .append("\nFirst frame presented: ").append(presented ? "yes" : "no").append('\n');
        String pressure = tail(new File(data, MEMORY), 64).trim();
        if (!pressure.isEmpty()) report.append("\nLast critical memory warning (time ms, trim level): ").append(pressure).append('\n');
        if (renderer != null) report.append("\nRenderer failure (OpenGL ES): kind=").append(renderer[0]).append(", ms since the first frame=")
                .append(renderer[1]).append(", renderer failure: ").append(renderer[2]).append('\n');
        if (!nativeReason.isEmpty()) report.append('\n').append(nativeReason).append('\n');
        if (!javaReason.isEmpty()) report.append("\nJava failure:\n").append(javaReason).append('\n');
        File tombstone = new File(data, TOMBSTONE);
        if (exit != null) ExitHistory.append(report, exit, tombstone);
        else {
            Files.deleteIfExists(tombstone.toPath());
            if (nativeReason.isEmpty() && javaReason.isEmpty())
                report.append("\nSession ended without an exit reason. A user stop, a system kill and a crash cannot be distinguished.\n");
        }
        report.append("\n--- stronghold.log (tail) ---\n").append(tail(new File(data, "stronghold.log"), LOG_LIMIT));
        write(new File(data, REPORT), report.toString());
        new AtomicFile(state).delete();
        return false;
    }

    static String tail(File path, int limit) throws IOException {
        if (!path.isFile()) return "";
        try (RandomAccessFile input = new RandomAccessFile(path, "r")) {
            long length = input.length();
            int size = (int)Math.min(length, limit);
            input.seek(length - size);
            byte[] bytes = new byte[size];
            input.readFully(bytes);
            return new String(bytes, StandardCharsets.UTF_8);
        }
    }

    private static void write(File path, String text) throws IOException {
        AtomicFile file = new AtomicFile(path);
        FileOutputStream out = file.startWrite();
        try {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            file.finishWrite(out);
        } catch (IOException error) {
            file.failWrite(out);
            throw error;
        }
    }

    /** Keep the Android 11 class outside the Android 9 code path. */
    private static final class ExitHistory {
        static ApplicationExitInfo find(ActivityManager manager, String packageName, String process, int pid, long started) {
            for (ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(packageName, pid, 8))
                if (exit.getTimestamp() >= started && process.equals(exit.getProcessName())) return exit;
            return null;
        }

        static String cause(ApplicationExitInfo exit) {
            switch (exit.getReason()) {
            case ApplicationExitInfo.REASON_ANR: return ANR;
            case ApplicationExitInfo.REASON_LOW_MEMORY: return LOW_MEMORY;
            case ApplicationExitInfo.REASON_CRASH: return JAVA_FAILURE;
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return NATIVE_SIGNAL;
            case ApplicationExitInfo.REASON_EXIT_SELF: return EXIT_CODE;
            default: return SYSTEM_KILL;
            }
        }

        /** The signal of a native crash, the exit code, or "reason N, status M" of a kill by the system. */
        static String detail(ApplicationExitInfo exit) {
            int reason = exit.getReason();
            if (reason == ApplicationExitInfo.REASON_CRASH_NATIVE || reason == ApplicationExitInfo.REASON_EXIT_SELF)
                return Integer.toString(exit.getStatus());
            if (reason == ApplicationExitInfo.REASON_ANR || reason == ApplicationExitInfo.REASON_LOW_MEMORY
                    || reason == ApplicationExitInfo.REASON_CRASH) return "";
            return "reason " + reason + ", status " + exit.getStatus();
        }

        static boolean intentional(ApplicationExitInfo exit) {
            int reason = exit.getReason();
            return reason == ApplicationExitInfo.REASON_USER_REQUESTED || reason == ApplicationExitInfo.REASON_USER_STOPPED ||
                   reason == ApplicationExitInfo.REASON_PACKAGE_UPDATED || reason == ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE ||
                   (reason == ApplicationExitInfo.REASON_EXIT_SELF && exit.getStatus() == 0);
        }

        static void append(StringBuilder report, ApplicationExitInfo exit, File tombstone) throws IOException {
            report.append("\nAndroid exit reason: ").append(exit.getReason()).append(", status: ").append(exit.getStatus())
                    .append(", time: ").append(Instant.ofEpochMilli(exit.getTimestamp()))
                    .append("\nLast PSS (KiB): ").append(exit.getPss()).append("\nLast RSS (KiB): ").append(exit.getRss()).append('\n');
            if (exit.getDescription() != null) report.append(exit.getDescription()).append('\n');
            Files.deleteIfExists(tombstone.toPath());
            try (InputStream trace = exit.getTraceInputStream()) {
                if (trace == null) return;
                if (exit.getReason() == ApplicationExitInfo.REASON_ANR) {
                    byte[] buffer = new byte[LOG_LIMIT];
                    int offset = 0, size;
                    while (offset < buffer.length && (size = trace.read(buffer, offset, buffer.length - offset)) > 0) offset += size;
                    report.append("\n--- Android ANR trace (head) ---\n")
                            .append(new String(buffer, 0, offset, StandardCharsets.UTF_8)).append('\n');
                } else if (Build.VERSION.SDK_INT >= 31 && exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                    saveTombstone(trace, tombstone);
                    report.append("Native tombstone: ").append(tombstone.getName()).append('\n');
                }
            } catch (IOException | RuntimeException error) { report.append("Trace unavailable: ").append(error).append('\n'); }
        }

        private static void saveTombstone(InputStream trace, File target) throws IOException {
            AtomicFile file = new AtomicFile(target);
            FileOutputStream out = file.startWrite();
            try {
                byte[] buffer = new byte[8192];
                int size, total = 0;
                while ((size = trace.read(buffer)) > 0) {
                    total += size;
                    if (total > TOMBSTONE_LIMIT) throw new IOException("Tombstone exceeds size limit");
                    out.write(buffer, 0, size);
                }
                file.finishWrite(out);
            } catch (IOException error) {
                file.failWrite(out);
                throw error;
            }
        }
    }
}
