package org.stronghold.port;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Properties;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Private, reversible asset overlays. Original game installations are never edited. */
final class GameMods {
    static final class Pack {
        final File folder;
        final String id, name, edition;
        Pack(File folder, Properties properties) throws IOException {
            this.folder = folder;
            id = properties.getProperty("id", ""); name = properties.getProperty("name", "");
            edition = properties.getProperty("edition", "");
            if (!id.matches("[a-z0-9][a-z0-9_.-]{0,63}") || id.equals(".") || id.equals("..") ||
                    name.trim().isEmpty() || name.length() > 120 || name.matches(".*\\p{Cntrl}.*") ||
                    !(edition.equals("classic") || edition.equals("extreme") || edition.equals("crusader") || edition.equals("stronghold")))
                throw new IOException(I18n.t("Некорректный mod.properties: нужны id, name и edition"));
        }
        boolean supports(String game) { return edition.equals(game) || edition.equals("crusader") && !game.equals("stronghold"); }
        File assets() { return new File(folder, "files"); }
    }
    static Pack read(File folder) throws IOException {
        File manifest = new File(folder, "mod.properties");
        if (!manifest.isFile() || manifest.length() > 8192) throw new IOException(I18n.t("Нет mod.properties"));
        Properties properties = new Properties();
        try (InputStream input = new FileInputStream(manifest)) {
            properties.load(new java.io.InputStreamReader(input, StandardCharsets.UTF_8));
        }
        Pack pack = new Pack(folder, properties);
        if (!pack.assets().isDirectory()) throw new IOException(I18n.t("В моде нет папки files"));
        return pack;
    }
    static List<Pack> packs(File storage, String edition) throws IOException {
        List<Pack> packs = new ArrayList<>();
        File[] folders = storage.listFiles(File::isDirectory);
        if (folders != null) for (File folder : folders) {
            if (folder.getName().startsWith(".")) continue;
            Pack pack = read(folder);
            if (!pack.id.equals(folder.getName())) throw new IOException(I18n.t("ID мода не совпадает с папкой"));
            if (pack.supports(edition)) packs.add(pack);
        }
        Collections.sort(packs, (a, b) -> a.id.compareTo(b.id));
        return packs;
    }
    static boolean assetPath(String path) {
        if (path.isEmpty() || path.indexOf('\\') >= 0 || path.startsWith("/") ||
                path.matches(".*[:\\p{Cntrl}].*")) return false;
        for (String component : path.split("/", -1)) if (component.isEmpty() || component.equals(".") || component.equals("..")) return false;
        String lower = path.toLowerCase(Locale.ROOT);
        if (lower.matches(".*\\.(exe|dll|lua|asi|cfg|ini|bat|cmd|sh)$")) return false;
        int slash = lower.indexOf('/');
        String root = slash < 0 ? lower : lower.substring(0, slash);
        if (("|aiv|binks|fx|gfx|gfx8|gm|maps|mapsextreme|").contains("|" + root + "|")) return true;
        return lower.matches(".*\\.(tex|ani|cur|bmp|csv|mlb)$");
    }
    static Pack install(InputStream input, File storage, java.util.function.BooleanSupplier cancelled) throws IOException {
        if (!storage.isDirectory() && !storage.mkdirs()) throw new IOException(I18n.t("Не удалось создать папку модов"));
        File staging = Files.createTempDirectory(storage.toPath(), ".import-").toFile();
        boolean installed = false;
        try (ZipInputStream zip = new ZipInputStream(input, StandardCharsets.UTF_8)) {
            long bytes = 0; int count = 0; Set<String> seen = new HashSet<>(); byte[] buffer = new byte[65536];
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                if (cancelled.getAsBoolean()) throw new java.io.InterruptedIOException(I18n.t("Установка мода отменена"));
                String name = entry.getName();
                if (++count > 20000 || name.length() > 500) throw new IOException(I18n.t("В моде слишком много файлов"));
                if (entry.isDirectory()) name = name.substring(0, name.length()-1);
                boolean manifest = name.equals("mod.properties");
                boolean files = name.equals("files") || name.startsWith("files/") && assetPath(name.substring(6));
                if ((!manifest && !files) || name.equals("files") && !entry.isDirectory() ||
                        manifest && entry.isDirectory() || !seen.add(name.toLowerCase(Locale.ROOT)))
                    throw new IOException(I18n.t("Недопустимый путь мода: ") + name);
                File output = new File(staging, name);
                if (!output.getCanonicalPath().startsWith(staging.getCanonicalPath() + File.separator)) throw new IOException(I18n.t("Путь вне папки мода"));
                if (entry.isDirectory()) { if (!output.mkdirs() && !output.isDirectory()) throw new IOException(I18n.t("Не удалось создать папку")); continue; }
                if (!output.getParentFile().isDirectory() && !output.getParentFile().mkdirs()) throw new IOException(I18n.t("Не удалось создать папку"));
                long fileBytes = 0;
                try (FileOutputStream file = new FileOutputStream(output)) {
                    for (int n; (n = zip.read(buffer)) != -1;) {
                        if (cancelled.getAsBoolean()) throw new java.io.InterruptedIOException(I18n.t("Установка мода отменена"));
                        bytes += n; fileBytes += n;
                        if (bytes > 2L*1024*1024*1024 || manifest && fileBytes > 8192 || storage.getUsableSpace() < 16L*1024*1024)
                            throw new IOException(I18n.t("Слишком большой мод или недостаточно места"));
                        file.write(buffer, 0, n);
                    }
                }
                zip.closeEntry();
            }
            Pack pack = read(staging); File target = new File(storage, pack.id);
            if (target.exists()) throw new IOException(I18n.t("Мод с ID ") + pack.id + I18n.t(" уже установлен; сначала удалите старую версию"));
            Files.move(staging.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
            installed = true; return read(target);
        } finally { if (!installed) delete(storage, staging); }
    }
    static void delete(File storage, File folder) throws IOException {
        if (!folder.getCanonicalFile().getParentFile().equals(storage.getCanonicalFile())) throw new IOException(I18n.t("Путь вне папки модов"));
        try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(folder.toPath())) {
            java.nio.file.Path[] entries = paths.sorted(java.util.Comparator.reverseOrder()).toArray(java.nio.file.Path[]::new);
            for (java.nio.file.Path entry : entries) Files.delete(entry);
        }
    }
    static void configure(File destination, String revision, Set<String> features, List<Pack> packs, Set<String> enabled) throws IOException {
        List<String> roots = new ArrayList<>();
        for (Pack pack : packs) if (enabled.contains(pack.id)) roots.add(pack.assets().getCanonicalPath());
        writeConfiguration(destination, revision, features, roots);
    }
    private static List<String> configuration(File source) throws IOException {
        if (!source.exists()) return Collections.emptyList();
        if (source.length() > 128*1024) throw new IOException(I18n.t("Слишком большой файл настроек UCP"));
        return Files.readAllLines(source.toPath(), StandardCharsets.UTF_8);
    }
    static Set<String> features(File source) throws IOException {
        Set<String> result = new HashSet<>();
        for (String line : configuration(source)) if (line.matches("[a-z][a-z0-9_]{0,95}=1")) result.add(line.substring(0, line.length()-2));
        return result;
    }
    static void configureFeatures(File destination, String revision, Set<String> features) throws IOException {
        List<String> lines = configuration(destination), roots = new ArrayList<>();
        if (!lines.isEmpty() && !lines.get(0).equals("UCP1 " + revision)) throw new IOException(I18n.t("Несовместимая версия настроек UCP"));
        for (String line : lines) if (line.startsWith("@")) roots.add(line.substring(1));
        writeConfiguration(destination, revision, features, roots);
    }
    private static void writeConfiguration(File destination, String revision, Set<String> features, List<String> roots) throws IOException {
        if (!revision.matches("[0-9a-f]{40}")) throw new IOException(I18n.t("Некорректная версия UCP"));
        File temporary = new File(destination.getParentFile(), destination.getName()+".tmp");
        try {
            try (FileOutputStream file = new FileOutputStream(temporary); OutputStreamWriter writer = new OutputStreamWriter(file, StandardCharsets.UTF_8)) {
                writer.write("UCP1 " + revision + "\n");
                for (String feature : new java.util.TreeSet<>(features)) {
                    if (!feature.matches("[a-z][a-z0-9_]{0,95}")) throw new IOException(I18n.t("Некорректный ID UCP"));
                    writer.write(feature+"=1\n");
                }
                int count = 0;
                for (String path : roots) {
                    if (++count > 32 || !new File(path).isDirectory() || !new File(path).isAbsolute() || path.length() > 3000 || path.matches(".*[\\r\\n].*"))
                        throw new IOException(I18n.t("Не удалось подключить папку мода"));
                    writer.write("@"+path+"\n");
                }
                writer.flush(); file.getFD().sync();
            }
            Files.move(temporary.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temporary.toPath()); }
    }
}
