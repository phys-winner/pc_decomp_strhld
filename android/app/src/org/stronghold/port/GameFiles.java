package org.stronghold.port;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.HashSet;

/** File checks shared by the launcher and direct activity entry points. */
final class GameFiles {
    static final String[] EDITIONS = {"classic", "extreme", "stronghold"};
    static final String DEFAULT_GAME = "/sdcard/Stronghold/game";
    /* The original Stronghold HD is a separate installation (tools/push_data.py --edition stronghold). */
    static final String DEFAULT_STRONGHOLD = "/sdcard/Stronghold/stronghold-hd";
    static boolean original(String edition) { return "stronghold".equals(edition); }
    static String pathKey(String edition) {
        return original(edition) ? "strongholdGame" : "extreme".equals(edition) ? "extremeGame" : "game";
    }
    static String defaultPath(String edition) { return original(edition) ? DEFAULT_STRONGHOLD : DEFAULT_GAME; }
    /** A missing Extreme preference inherits the old shared Crusader setting. */
    static String[] savedPaths(String classic, String extreme, String stronghold) {
        String game = classic == null ? DEFAULT_GAME : classic.trim();
        return new String[] {game, extreme == null ? game : extreme.trim(), stronghold == null ? DEFAULT_STRONGHOLD : stronghold.trim()};
    }
    static final class Scan {
        final String[] paths, problems, warnings;
        String discoveryProblem;
        Scan(String[] saved, boolean hashes) {
            paths = saved.clone(); problems = new String[EDITIONS.length]; warnings = new String[EDITIONS.length];
            for (int i = 0; i < EDITIONS.length; i++) {
                Check check = check(new File(paths[i]), EDITIONS[i], hashes);
                problems[i] = check.error; warnings[i] = check.warning;
            }
        }
        boolean complete() { for (String problem : problems) if (problem != null) return false; return true; }
    }
    /** Detect supported editions in a folder or common parent, preserving other working installations. */
    static Scan scan(String[] saved, File source, boolean replaceMatching, boolean hashes) {
        Scan result = new Scan(saved, hashes);
        if (source == null || (!replaceMatching && result.complete())) return result;
        if (!source.isAbsolute() || source.getPath().getBytes(StandardCharsets.UTF_8).length >= 512) {
            result.discoveryProblem = validate(source, "classic", false); return result;
        }
        ArrayDeque<File> folders = new ArrayDeque<>(); ArrayDeque<Integer> depths = new ArrayDeque<>();
        HashSet<String> visited = new HashSet<>(); boolean[] found = new boolean[EDITIONS.length];
        folders.add(source); depths.add(0);
        String boundary;
        try { boundary = source.getCanonicalPath(); }
        catch (IOException failure) { result.discoveryProblem = "Не удалось открыть папку: " + failure.getMessage(); return result; }
        String prefix = boundary.endsWith(File.separator) ? boundary : boundary + File.separator;
        int searched = 0;
        while (!folders.isEmpty() && searched++ < 512) {
            File folder = folders.removeFirst(); int depth = depths.removeFirst();
            String canonical;
            try { canonical = folder.getCanonicalPath(); } catch (IOException ignored) { continue; }
            if ((!canonical.equals(boundary) && !canonical.startsWith(prefix)) || !visited.add(canonical)) continue;
            boolean gfx = new File(folder, "gfx").isDirectory();
            for (int i = 0; gfx && i < EDITIONS.length; i++) {
                if (found[i] || !new File(folder, original(EDITIONS[i]) ? "sh.tex" : "cr.tex").isFile()) continue;
                Check check = check(folder, EDITIONS[i], hashes);
                String problem = check.error;
                if (problem != null) {
                    if (result.discoveryProblem == null) result.discoveryProblem = title(EDITIONS[i]) + ": " + problem;
                    continue;
                }
                found[i] = true;
                if (replaceMatching || result.problems[i] != null) {
                    try {
                        if (result.problems[i] != null || !canonical.equals(new File(result.paths[i]).getCanonicalPath()))
                            result.paths[i] = folder.getAbsolutePath();
                    } catch (IOException ignored) { result.paths[i] = folder.getAbsolutePath(); }
                    result.problems[i] = null;
                    result.warnings[i] = check.warning;
                }
            }
            if (found[0] && found[1] && found[2]) break;
            if (depth >= 4) continue;
            File[] children = folder.listFiles(File::isDirectory);
            if (children != null) for (File child : children) { folders.add(child); depths.add(depth + 1); }
        }
        if (found[0] || found[1] || found[2]) result.discoveryProblem = null;
        else if (result.discoveryProblem == null)
            result.discoveryProblem = "В выбранной папке не найдены файлы поддерживаемой игры (sh.tex или cr.tex, gfx, карты, EXE и DLL).";
        return result;
    }
    static String executable(String edition) {
        return "classic".equals(edition) ? "Stronghold Crusader.exe" : original(edition) ? "Stronghold.exe" : "Stronghold_Crusader_Extreme.exe";
    }
    /** The edition's ARM64 library (tools/build_android.py --shared --edition ...). */
    static String library(String edition) {
        return "classic".equals(edition) ? "stronghold_classic" : original(edition) ? "stronghold_sh" : "stronghold";
    }
    /** The edition's profile folder in /sdcard/Stronghold. */
    static String userdata(String edition) {
        return "classic".equals(edition) ? "userdata-classic" : original(edition) ? "userdata-stronghold" : "userdata";
    }
    static String title(String edition) {
        return "classic".equals(edition) ? "Crusader" : original(edition) ? "Stronghold HD" : "Crusader Extreme";
    }
    private static String lifted(String edition) { return original(edition) ? "lifted-stronghold" : "lifted"; }
    static File images(File game, String edition) {
        if (new File(game, executable(edition)).isFile()) return game;
        File sibling = new File(game.getParentFile(), lifted(edition));
        if (new File(sibling, executable(edition)).isFile()) return sibling;
        /* A game unpacked into a new folder can still use the deployed PE copies. */
        File shared = new File(new File(android.os.Environment.getExternalStorageDirectory(), "Stronghold"), lifted(edition));
        return new File(shared, executable(edition)).isFile() ? shared : sibling;
    }
    static File pending(File files, String edition) { return new File(files, "launch-" + edition + ".pending"); }
    static void rememberSuccess(File files, String edition, String directory) throws IOException {
        File temporary = new File(files, "last-game-" + edition + ".tmp");
        try (DataOutputStream output = new DataOutputStream(new FileOutputStream(temporary))) {
            output.writeUTF(edition); output.writeUTF(directory);
        }
        Files.move(temporary.toPath(), new File(files, "last-game").toPath(),
                StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }
    static String[] lastSuccess(File files) {
        try (DataInputStream input = new DataInputStream(new FileInputStream(new File(files, "last-game")))) {
            String edition = input.readUTF(), directory = input.readUTF();
            if ((!"classic".equals(edition) && !"extreme".equals(edition) && !original(edition))
                    || !new File(directory).isAbsolute()) return null;
            return new String[] {edition, directory};
        } catch (IOException invalid) { return null; }
    }
    static File uniqueFile(File file) {
        String name = file.getName(); int dot = name.lastIndexOf('.');
        String base = dot > 0 ? name.substring(0, dot) : name, extension = dot > 0 ? name.substring(dot) : "";
        File candidate = file;
        for (int number = 2; candidate.exists(); number++) candidate = new File(file.getParentFile(), base + " (" + number + ")" + extension);
        return candidate;
    }
    static File archiveStaging(File destination, int ordinal) {
        /* Android shared storage rejects these names before recovery can rename them. */
        if (destination.getName().matches(".*[<>:\"|?*\\p{Cntrl}].*"))
            destination = new File(destination.getParentFile(), ".stronghold-import-" + ordinal + ".tmp");
        return uniqueFile(destination);
    }
    static String archiveMapName(String name, long crc) {
        int slash = name.lastIndexOf('/'); String basename = name.substring(slash + 1);
        if (!basename.toLowerCase(java.util.Locale.ROOT).endsWith(".map")
                || (basename.indexOf('?') < 0 && basename.indexOf('\ufffd') < 0)) return name;
        return name.substring(0, slash + 1) + String.format(java.util.Locale.ROOT, "Карта %08X.map", crc);
    }
    static final class Check {
        final String error, warning;
        Check(String error, String warning) { this.error = error; this.warning = warning; }
    }
    static String validate(File game, String edition, boolean hashes) { return check(game, edition, hashes).error; }
    static Check check(File game, String edition, boolean hashes) {
        if (!game.isAbsolute() || game.getPath().getBytes(StandardCharsets.UTF_8).length >= 512)
            return new Check("Укажите абсолютный путь к папке игры (короче 512 байт).", null);
        if (!game.isDirectory() || !game.canRead()) return new Check("Папка игры недоступна: " + game, null);
        String tex = original(edition) ? "sh.tex" : "cr.tex";
        String[] required = original(edition) ? new String[] {"fx", "gfx", "gm", "maps", tex}
                : new String[] {"aiv", "fx", "gfx", "gm", "maps", tex};
        for (String name : required) {
            File item = new File(game, name);
            if (!item.canRead() || (name.equals(tex) ? !item.isFile() : !item.isDirectory()))
                return new Check("В папке игры отсутствует или недоступен: " + name, null);
        }
        if ("extreme".equals(edition) && !new File(game, "mapsExtreme").isDirectory())
            return new Check("Для Extreme требуется папка mapsExtreme.", null);
        File images = images(game, edition);
        String[] names = {executable(edition), "Mss32.dll", "binkw32.dll"};
        String[] expected = {"classic".equals(edition)
                ? "e7c7afb39653a45e2f66d5f5c17f075749e3637f4a2ef30de4c8aca24843e69a"
                : original(edition) ? "8b581f48df4d14b65b596f3a34ca2ee37b388ed93e7b1e505cceb0b1dadbcede"
                : "0c90204f7bf622b768cc1fd9ebc0ec5c6cf8e60ea84911c0fdc7ffcbb2119ba8",
                "2a515199cc7a2e1ccd8d756efdaab07cedd5c3052309ef1c7499c2560e2097bd",
                "ee68dcf51e8e3b482f9fb5ef617f44a2d9e970d03f3ed21fe26d40cd63c57a48"};
        String warning = null;
        for (int i = 0; i < names.length; i++) {
            File image = new File(images, names[i]);
            if (!image.isFile() || !image.canRead()) return new Check("Не найден " + image + "\nВыберите распакованную игру с EXE и DLL или сохраните соседнюю папку " + lifted(edition) + ".", null);
            if (!hashes) continue;
            try (FileInputStream input = new FileInputStream(image)) {
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                byte[] buffer = new byte[65536]; int count;
                while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
                StringBuilder actual = new StringBuilder();
                for (byte value : digest.digest()) actual.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
                if (!actual.toString().equals(expected[i])) {
                    if (i == 0) warning = "Версия EXE отличается от использованной при сборке порта: " + image
                            + "\nСовместимость не гарантируется: возможны ошибки или вылеты.";
                    else return new Check("Неподдерживаемая версия файла: " + names[i] + ". Требуются оригинальные файлы "
                            + (original(edition) ? "Stronghold HD (GOG)" : "Crusader HD") + ", использованные при сборке порта.", null);
                }
            } catch (IOException | NoSuchAlgorithmException error) { return new Check("Не удалось проверить " + image + ": " + error.getMessage(), null); }
        }
        return new Check(null, warning);
    }
}
