package org.stronghold.port;

import android.content.Context;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.InterruptedIOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** Reads GOG installers as data using the bundled ARM64 innoextract. */
final class GogInstaller {
    static final class Info {
        final File setup;
        final String game;
        final int files, chunks, compressed;
        final long bytes;
        final boolean split;
        final List<String> roots;
        Info(File s, String g, int f, long b, int c, int z, boolean external, List<String> r) {
            setup=s; game=g; files=f; bytes=b; chunks=c; compressed=z; split=external; roots=r;
        }
    }
    interface Progress { void update(long bytes, long total, int files); }
    private static String executable(Context context) {
        return new File(context.getApplicationInfo().nativeLibraryDir, "libinnoextract.so").getAbsolutePath();
    }
    static Info parse(File setup, List<String> lines) throws IOException {
        Info result = null; List<String> roots = new ArrayList<>();
        for (String line : lines) {
            if (line.startsWith("GOG1\t")) {
                String[] values = line.split("\t", -1);
                if (values.length != 7 || result != null) throw new IOException(I18n.t("Некорректные данные установщика"));
                try {
                    String game = "1207658712".equals(values[1]) ? "stronghold" : "1207658713".equals(values[1]) ? "crusader" : null;
                    int files = Integer.parseInt(values[2]), chunks = Integer.parseInt(values[4]), compressed = Integer.parseInt(values[5]);
                    long bytes = Long.parseLong(values[3]);
                    if (game == null || files < 1 || files > 100000 || chunks < 1 || chunks > files || compressed < 0 || compressed > chunks ||
                            bytes < 1 || bytes > 8L * 1024 * 1024 * 1024 || !("0".equals(values[6]) || "1".equals(values[6])))
                        throw new IOException(I18n.t("Неподдерживаемый установщик GOG"));
                    result = new Info(setup, game, files, bytes, chunks, compressed, "1".equals(values[6]), roots);
                } catch (NumberFormatException invalid) { throw new IOException(I18n.t("Некорректный размер установщика"), invalid); }
            } else if (line.startsWith("ROOT\t")) {
                String name = line.substring(5);
                if (name.isEmpty() || name.equals(".") || name.equals("..") || name.matches(".*[\\\\/:\\p{Cntrl}].*") || roots.size() >= 256)
                    throw new IOException(I18n.t("Недопустимое имя файла установщика"));
                roots.add(name);
            }
        }
        if (result == null || roots.isEmpty()) throw new IOException(I18n.t("Не удалось прочитать установщик GOG"));
        return result;
    }
    static Info inspect(Context context, File setup) throws IOException {
        if (!setup.isFile() || !setup.canRead() || setup.length() > 8L * 1024 * 1024 * 1024) throw new IOException(I18n.t("Установщик недоступен"));
        try (FileInputStream input = new FileInputStream(setup)) {
            if (input.read() != 'M' || input.read() != 'Z') throw new IOException(I18n.t("Установщик повреждён или ещё не загружен"));
        }
        Process process = new ProcessBuilder(executable(context), "--port-info", setup.getAbsolutePath()).redirectErrorStream(true).start();
        try {
            if (!process.waitFor(30, TimeUnit.SECONDS)) throw new IOException(I18n.t("Не удалось прочитать установщик за 30 секунд"));
            List<String> lines = new ArrayList<>();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream(), "UTF-8"))) {
                for (String line; (line = reader.readLine()) != null; ) {
                    if (lines.size() >= 1024 || line.length() > 4096) throw new IOException(I18n.t("Слишком много данных установщика"));
                    lines.add(line);
                }
            }
            if (process.exitValue() != 0) throw new IOException(I18n.t("Не удалось прочитать GOG EXE. Проверьте загрузку EXE и соседних BIN.\n") + String.join("\n", lines));
            return parse(setup, lines);
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new InterruptedIOException(); }
        finally { process.destroy(); }
    }
    static void extract(Context context, Info info, File target, BooleanSupplier cancelled, Consumer<Process> active, Progress progress) throws IOException {
        if (!target.isDirectory() || target.getUsableSpace() < info.bytes + 64L * 1024 * 1024) throw new IOException(I18n.t("Недостаточно места для установки"));
        if (cancelled.getAsBoolean()) throw new InterruptedIOException();
        List<String> command = new ArrayList<>();
        java.util.Collections.addAll(command, executable(context), "--extract", "--exclude-temp", "--color=0", "--progress=0", "--no-extract-unknown",
                "--output-dir", target.getAbsolutePath());
        for (String directory : new String[]{"aiv", "binks", "fx", "gfx", "gfx8", "gm", "maps", "mapsExtreme"}) {
            command.add("--include"); command.add("/" + directory);
        }
        for (String root : info.roots) { command.add("--include"); command.add("/" + root); }
        command.add(info.setup.getAbsolutePath());
        Process process = new ProcessBuilder(command).redirectErrorStream(true).start(); active.accept(process);
        long bytes = 0; int files = 0; String last = "";
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream(), "UTF-8"))) {
            for (String line; (line = reader.readLine()) != null; ) {
                if (cancelled.getAsBoolean()) throw new InterruptedIOException();
                if (line.length() > 4096) throw new IOException(I18n.t("Слишком длинная строка установщика"));
                if (line.startsWith(" - \"")) {
                    int end = line.indexOf('"', 4);
                    if (end > 4) {
                        File file = new File(target, line.substring(4, end).replace('\\', '/'));
                        if (!file.getCanonicalPath().startsWith(target.getCanonicalPath() + File.separator)) throw new IOException("Недопустимый путь установщика");
                        bytes += file.length(); progress.update(bytes, info.bytes, ++files);
                    }
                }
                if (!line.trim().isEmpty()) last = line;
            }
            if (cancelled.getAsBoolean()) throw new InterruptedIOException();
            if (process.waitFor() != 0) throw new IOException("Установка GOG не завершена: " + last + "\nПроверьте все BIN рядом с EXE.");
            String error = GameFiles.validate(target, "stronghold".equals(info.game) ? "stronghold" : "classic", true);
            if (error != null) throw new IOException(error);
            progress.update(info.bytes, info.bytes, files);
        } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new InterruptedIOException(); }
        finally { process.destroy(); active.accept(null); }
    }
}
