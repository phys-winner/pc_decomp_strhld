package org.stronghold.port;

/** The game's own Windows cursor images (.cur, and the first frame of an
 * .ani), decoded without Android so the reader can be tested on a PC. The
 * original never draws a cursor: Windows draws the one it passes to SetCursor. */
final class GameCursor {
    final int width, height, hotX, hotY;
    final int[] argb;
    private GameCursor(int w, int h, int x, int y, int[] pixels) { width = w; height = h; hotX = x; hotY = y; argb = pixels; }

    private static int u16(byte[] d, int at) { return (d[at] & 0xFF) | (d[at + 1] & 0xFF) << 8; }
    private static int s32(byte[] d, int at) { return u16(d, at) | u16(d, at + 2) << 16; }
    private static boolean tag(byte[] d, int at, String name) {
        if (at < 0 || at + 4 > d.length) return false;
        for (int i = 0; i < 4; i++) if (d[at + i] != (byte)name.charAt(i)) return false;
        return true;
    }

    /** An .ani (RIFF ACON: first "icon" frame), .cur or .ico image; null if unreadable. */
    static GameCursor decode(byte[] d) {
        if (d == null) return null;
        try {
            if (tag(d, 0, "RIFF") && tag(d, 8, "ACON")) {
                int frame = frame(d, 12, d.length);
                return frame < 0 ? null : icon(d, frame + 8, Math.min(d.length, frame + 8 + s32(d, frame + 4)));
            }
            return icon(d, 0, d.length);
        } catch (RuntimeException broken) { return null; }
    }

    /* The offset of the first "icon" chunk, looking into LIST chunks. */
    private static int frame(byte[] d, int at, int end) {
        while (at + 8 <= end) {
            int size = s32(d, at + 4);
            if (size < 0 || at + 8L + size > end) return -1;
            if (tag(d, at, "icon")) return at;
            if (tag(d, at, "LIST")) { int inner = frame(d, at + 12, at + 8 + size); if (inner >= 0) return inner; }
            at += 8 + size + (size & 1);
        }
        return -1;
    }

    /* ICONDIR, its first entry (a cursor keeps its hotspot there) and the DIB. */
    private static GameCursor icon(byte[] d, int base, int end) {
        int type = u16(d, base + 2);
        if (u16(d, base) != 0 || (type != 1 && type != 2) || u16(d, base + 4) < 1) return null;
        int entry = base + 6, hotX = type == 2 ? u16(d, entry + 4) : 0, hotY = type == 2 ? u16(d, entry + 6) : 0;
        int offset = base + s32(d, entry + 12);
        if (offset < base || offset + 40 > end) return null;
        int header = s32(d, offset), w = s32(d, offset + 4), h2 = s32(d, offset + 8), bits = u16(d, offset + 14);
        int compression = s32(d, offset + 16), colors = s32(d, offset + 32);
        int h = Math.abs(h2) / 2;
        if (header < 40 || w <= 0 || w > 256 || h <= 0 || h > 256 || compression != 0) return null;
        if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) return null;
        int paletteSize = bits <= 8 ? (colors > 0 ? colors : 1 << bits) : 0;
        int palette = offset + header, pixels = palette + 4 * paletteSize;
        int stride = ((w * bits + 31) / 32) * 4, maskStride = ((w + 31) / 32) * 4, mask = pixels + stride * h;
        if (mask + (bits == 32 ? 0 : maskStride * h) > end) return null;
        boolean alpha = false;
        int[] out = new int[w * h];
        for (int y = 0; y < h; y++) {
            int row = pixels + stride * (h2 > 0 ? h - 1 - y : y);
            for (int x = 0; x < w; x++) {
                int color;
                if (bits == 32) {
                    int at = row + 4 * x;
                    color = (d[at + 3] & 0xFF) << 24 | (d[at + 2] & 0xFF) << 16 | (d[at + 1] & 0xFF) << 8 | (d[at] & 0xFF);
                    alpha |= (color >>> 24) != 0;
                } else if (bits == 24) {
                    int at = row + 3 * x;
                    color = 0xFF000000 | (d[at + 2] & 0xFF) << 16 | (d[at + 1] & 0xFF) << 8 | (d[at] & 0xFF);
                } else {
                    int index = (d[row + x * bits / 8] & 0xFF) >> (8 - bits - (x * bits) % 8) & ((1 << bits) - 1);
                    int at = palette + 4 * Math.min(index, paletteSize - 1);
                    color = 0xFF000000 | (d[at + 2] & 0xFF) << 16 | (d[at + 1] & 0xFF) << 8 | (d[at] & 0xFF);
                }
                out[y * w + x] = color;
            }
        }
        /* The AND mask marks transparency; a 32-bit image with alpha needs none. */
        if (bits != 32 || !alpha) for (int y = 0; y < h; y++) {
            int row = mask + maskStride * (h2 > 0 ? h - 1 - y : y);
            for (int x = 0; x < w; x++) {
                boolean clear = row + x / 8 < end && ((d[row + x / 8] >> (7 - x % 8)) & 1) != 0;
                if (clear) out[y * w + x] = 0; else if (bits == 32) out[y * w + x] |= 0xFF000000;
            }
        }
        return new GameCursor(w, h, Math.min(hotX, w - 1), Math.min(hotY, h - 1), out);
    }
}
