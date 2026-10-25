package org.stronghold.port;

import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Checks every corrected ZIP name, size, CRC and UTF-8 flag against the user's source folder. */
public final class ArchiveImportSelfcheck {
    public static void main(String[] args) throws Exception {
        Path source = new File(args[1]).toPath(); HashSet<String> remaining = new HashSet<>();
        try (java.util.stream.Stream<Path> paths = Files.walk(source)) {
            paths.filter(Files::isRegularFile).forEach(path -> remaining.add(source.relativize(path).toString().replace('\\', '/')));
        }
        int files = 0, unicode = 0, maps = 0;
        /* ASCII fallback rejects any non-ASCII name lacking ZIP's UTF-8 flag. */
        try (ZipFile zip = new ZipFile(args[0], StandardCharsets.US_ASCII)) {
            java.util.Enumeration<? extends ZipEntry> entries = zip.entries();
            byte[] buffer = new byte[65536];
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement(); String name = entry.getName();
                if (entry.isDirectory()) continue;
                String prefix = source.getFileName().toString() + "/";
                String relative = name.startsWith(prefix) ? name.substring(prefix.length()) : name;
                assert name.indexOf('?') < 0 && name.indexOf('\ufffd') < 0 : name;
                assert remaining.remove(relative) : "Missing original or duplicate entry: " + name;
                Path original = source.resolve(relative); CRC32 crc = new CRC32();
                try (InputStream input = Files.newInputStream(original)) {
                    for (int count; (count = input.read(buffer)) >= 0;) crc.update(buffer, 0, count);
                }
                assert entry.getSize() == Files.size(original) && entry.getCrc() == crc.getValue() : name;
                assert GameFiles.archiveMapName(name, entry.getCrc()).equals(name);
                if (name.chars().anyMatch(c -> c > 127)) unicode++;
                if (relative.startsWith("maps/") && relative.endsWith(".map")) maps++;
                files++;
            }
        }
        assert remaining.isEmpty() : "ZIP omitted " + remaining.size() + " original files";
        assert files > 0 && maps > 0;
        System.out.println("UTF-8 archive: " + files + " files, " + maps + " maps, " + unicode
                + " Unicode names with UTF-8 flags; every name/size/CRC matches source PASS");
    }
}
