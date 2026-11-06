package org.stronghold.port;

import android.content.Context;
import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.zip.GZIPInputStream;

/** Converts known GOG layouts to the AOT engine ABI without changing the player's installation. */
final class GameImages {
    static File prepare(Context context, File game, String edition) throws IOException {
        File original = GameFiles.images(game, edition), executable = new File(original, GameFiles.executable(edition));
        GameCompatibility.Executable entry = GameCompatibility.executable(edition, GameFiles.sha256(executable));
        if (entry == null || entry.sha256.equals(entry.target)) return original;
        File cache = new File(context.getFilesDir(), "images/" + edition + "-" + entry.sha256);
        if (!cache.isDirectory() && !cache.mkdirs()) throw new IOException(I18n.t("Не удалось подготовить EXE"));
        File output = new File(cache, executable.getName());
        if (!output.isFile() || output.length() != entry.targetBytes || !GameFiles.sha256(output).equals(entry.target)) {
            File temporary = new File(cache, executable.getName() + ".tmp");
            try (InputStream patch = context.getAssets().open("compatibility/" + entry.sha256 + ".delta")) {
                normalize(executable, patch, temporary, entry);
                Files.move(temporary.toPath(), output.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } finally { Files.deleteIfExists(temporary.toPath()); }
        }
        for (String name : new String[]{"Mss32.dll", "binkw32.dll"}) {
            File source = new File(original, name), destination = new File(cache, name);
            if (!destination.isFile() || !GameFiles.sha256(destination).equals(GameFiles.sha256(source)))
                Files.copy(source.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
        return cache;
    }
    static void normalize(File source, InputStream patch, File output, GameCompatibility.Executable entry) throws IOException {
        DataInputStream header = new DataInputStream(patch);
        if (header.readInt() != 0x53484431) throw new IOException(I18n.t("Неверный формат адаптации EXE"));
        int before = Integer.reverseBytes(header.readInt()), after = Integer.reverseBytes(header.readInt());
        int count = Integer.reverseBytes(header.readInt());
        byte[] sourceHash = new byte[32], targetHash = new byte[32]; header.readFully(sourceHash); header.readFully(targetHash);
        if (before != entry.bytes || after != entry.targetBytes || count != Math.max(before, after) || count > 16 * 1024 * 1024 ||
                !hex(sourceHash).equals(entry.sha256) || !hex(targetHash).equals(entry.target) || source.length() != before ||
                !GameFiles.sha256(source).equals(entry.sha256)) throw new IOException(I18n.t("EXE не соответствует адаптации"));
        try (InputStream input = new BufferedInputStream(new FileInputStream(source));
             DataInputStream delta = new DataInputStream(new GZIPInputStream(patch));
             FileOutputStream target = new FileOutputStream(output)) {
            byte[] bytes = new byte[65536], changes = new byte[65536];
            for (int offset = 0; offset < count; offset += bytes.length) {
                int size = Math.min(bytes.length, count - offset), available = Math.max(0, Math.min(size, before - offset));
                Arrays.fill(bytes, (byte)0);
                for (int at = 0; at < available; ) {
                    int read = input.read(bytes, at, available - at);
                    if (read < 0) throw new IOException(I18n.t("EXE обрезан"));
                    at += read;
                }
                delta.readFully(changes, 0, size);
                for (int at = 0; at < size; at++) bytes[at] ^= changes[at];
                target.write(bytes, 0, Math.max(0, Math.min(size, after - offset)));
            }
            if (delta.read() != -1) throw new IOException(I18n.t("Лишние данные адаптации EXE"));
            target.getFD().sync();
        }
        if (output.length() != after || !GameFiles.sha256(output).equals(entry.target)) throw new IOException(I18n.t("Контрольная сумма адаптированного EXE не совпала"));
    }
    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte value : bytes) result.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }
}
