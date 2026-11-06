package org.stronghold.port;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/** Reads the original interface sprites directly from the selected game's GM1 files. */
final class GameIcons {
    final int width, height;
    final int[] argb;
    final boolean mirrored;
    private GameIcons(int w, int h, int[] pixels, boolean mirror) {
        width = w; height = h; argb = pixels; mirrored = mirror;
    }
    static String file(String id) {
        switch (id) {
            case "menu": case "text": case "settings": case "f5": case "f1": return "interface_buttons.gm1";
            case "help": case "zoom": case "rotate_left": case "rotate_right": return "interface_icons2.gm1";
            case "layout": case "relief": case "editor": return "mapedit_buttons.gm1";
            default: return null;
        }
    }
    static int frame(String id) {
        switch (id) {
            case "menu": return 31;
            case "text": case "f5": return 61;
            case "settings": return 25;
            case "f1": return 7;
            case "help": return 288;
            case "zoom": return 302;
            case "rotate_left": case "rotate_right": return 300;
            case "layout": return 42;
            case "relief": return 3;
            case "editor": return 0;
            default: return -1;
        }
    }
    private static long u32(RandomAccessFile input) throws IOException {
        return Integer.toUnsignedLong(Integer.reverseBytes(input.readInt()));
    }
    static GameIcons load(File game, String id) {
        String name = file(id);
        if (name == null) return null;
        File source = new File(new File(game, "gm"), name);
        if (!source.isFile()) {
            File[] folders = game.listFiles();
            if (folders != null) for (File folder : folders) if (folder.getName().equalsIgnoreCase("gm")) {
                File[] files = folder.listFiles();
                if (files != null) for (File candidate : files) if (candidate.getName().equalsIgnoreCase(name)) source = candidate;
            }
        }
        try (RandomAccessFile input = new RandomAccessFile(source, "r")) {
            input.seek(12); long count = u32(input);
            input.seek(20); long type = u32(input);
            int index = frame(id);
            long data = 5208 + count * 24;
            if (type != 1 || count < 1 || count > 4096 || index < 0 || index >= count || data > input.length()) return null;
            input.seek(5208 + index * 4L); long offset = u32(input);
            input.seek(5208 + count * 4 + index * 4L); long length = u32(input);
            input.seek(5208 + count * 8 + index * 16L);
            int width = Short.toUnsignedInt(Short.reverseBytes(input.readShort()));
            int height = Short.toUnsignedInt(Short.reverseBytes(input.readShort()));
            if (length < 1 || length > 1048576 || data + offset + length > input.length()) return null;
            byte[] encoded = new byte[(int)length]; input.seek(data + offset); input.readFully(encoded);
            return decode(encoded, width, height, "rotate_right".equals(id));
        } catch (IOException | SecurityException error) { return null; }
    }
    static GameIcons decode(byte[] encoded, int width, int height, boolean mirror) {
        if (encoded == null || width < 1 || height < 1 || width > 512 || height > 512) return null;
        int[] pixels = new int[width * height];
        int x = 0, y = 0, at = 0;
        while (at < encoded.length && y < height) {
            int token = encoded[at++] & 255, kind = token >>> 5, count = (token & 31) + 1;
            if (kind == 4) { x = 0; y++; continue; }
            if (kind > 2 || x + count > width) return null;
            if (kind == 1) { x += count; continue; }
            int bytes = kind == 0 ? count * 2 : 2;
            if (at + bytes > encoded.length) return null;
            int color = 0;
            for (int i = 0; i < count; i++) {
                if (i == 0 || kind == 0) {
                    color = (encoded[at] & 255) | (encoded[at + 1] & 255) << 8; at += 2;
                    color = 0xff000000 | ((color >>> 10 & 31) * 255 / 31) << 16 |
                            ((color >>> 5 & 31) * 255 / 31) << 8 | (color & 31) * 255 / 31;
                }
                pixels[y * width + x++] = color;
            }
        }
        // GM1 sprites may end with an extra empty scanline marker.
        while (at < encoded.length && (encoded[at] & 255) == 128) at++;
        if (at != encoded.length || !(y == height || y == height - 1 && x == width)) return null;
        return new GameIcons(width, height, pixels, mirror);
    }
}
