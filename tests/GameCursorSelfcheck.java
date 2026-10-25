package org.stronghold.port;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.file.Files;

/** Optional argument: a directory with the game's sword.ani, hand.ani, scimitar.ani and arrow.cur. */
public final class GameCursorSelfcheck {
    static void check(boolean ok, String name) { if (!ok) throw new AssertionError(name); }
    static void u16(ByteArrayOutputStream o, int v) { o.write(v); o.write(v >> 8); }
    static void u32(ByteArrayOutputStream o, int v) { u16(o, v); u16(o, v >>> 16); }
    static void tag(ByteArrayOutputStream o, String t) { for (char c : t.toCharArray()) o.write(c); }

    /* A 4x2 cursor: 8-bit palette (0 black, 1 red), bottom-up rows, an AND mask clearing the top-right pixel. */
    static byte[] cursor(int type, int hotX, int hotY) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        u16(o, 0); u16(o, type); u16(o, 1);
        o.write(4); o.write(2); o.write(0); o.write(0); u16(o, hotX); u16(o, hotY);
        int dib = 40 + 2 * 4 + 2 * 4 + 2 * 4;
        u32(o, dib); u32(o, 22);
        u32(o, 40); u32(o, 4); u32(o, 4); u16(o, 1); u16(o, 8); u32(o, 0); u32(o, 0); u32(o, 0); u32(o, 0); u32(o, 2); u32(o, 0);
        u32(o, 0x000000); u32(o, 0xFF0000);                              /* BGRx bytes: black, red */
        o.write(0); o.write(1); o.write(1); o.write(0);                   /* bottom row: black red red black */
        o.write(1); o.write(1); o.write(0); o.write(1);                   /* top row: red red black red */
        u32(o, 0);                                                        /* bottom mask row: all opaque */
        o.write(0x10); o.write(0); o.write(0); o.write(0);                /* top mask row: pixel 3 clear */
        return o.toByteArray();
    }
    static byte[] ani(byte[] icon) {
        ByteArrayOutputStream frames = new ByteArrayOutputStream();
        tag(frames, "fram"); tag(frames, "icon"); u32(frames, icon.length); frames.write(icon, 0, icon.length);
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        tag(o, "RIFF"); u32(o, 0); tag(o, "ACON");
        tag(o, "anih"); u32(o, 36); for (int i = 0; i < 9; i++) u32(o, i == 0 ? 36 : 1);
        tag(o, "LIST"); u32(o, frames.size()); byte[] f = frames.toByteArray(); o.write(f, 0, f.length);
        return o.toByteArray();
    }
    public static void main(String[] args) throws Exception {
        GameCursor c = GameCursor.decode(cursor(2, 1, 1));
        check(c != null && c.width == 4 && c.height == 2 && c.hotX == 1 && c.hotY == 1, "cursor size and hotspot");
        check(c.argb[0] == 0xFFFF0000 && c.argb[2] == 0xFF000000 && c.argb[3] == 0, "top row colours and AND-mask transparency");
        check(c.argb[4] == 0xFF000000 && c.argb[5] == 0xFFFF0000 && c.argb[7] == 0xFF000000, "bottom-up rows");
        GameCursor a = GameCursor.decode(ani(cursor(2, 3, 0)));
        check(a != null && a.width == 4 && a.hotX == 3 && a.hotY == 0 && a.argb[1] == 0xFFFF0000, "first .ani frame");
        GameCursor i = GameCursor.decode(cursor(1, 3, 1));
        check(i != null && i.hotX == 0 && i.hotY == 0, "an icon has no hotspot");
        byte[] broken = cursor(2, 0, 0); broken = java.util.Arrays.copyOf(broken, 40);
        check(GameCursor.decode(broken) == null && GameCursor.decode(new byte[] {1, 2, 3}) == null && GameCursor.decode(null) == null, "broken data is refused");
        String real = "";
        if (args.length > 0) {
            for (String name : new String[] {"sword.ani", "hand.ani", "scimitar.ani", "arrow.cur"}) {
                File file = new File(args[0], name);
                if (!file.isFile()) continue;
                GameCursor g = GameCursor.decode(Files.readAllBytes(file.toPath()));
                check(g != null && g.width == 32 && g.height == 32, name + " decodes");
                int opaque = 0; for (int p : g.argb) if ((p >>> 24) != 0) opaque++;
                check(opaque > 20 && opaque < 32 * 32, name + " has a shape and a transparent background");
                real += " " + name + "(" + opaque + " px, hotspot " + g.hotX + "," + g.hotY + ")";
            }
        }
        System.out.println("GameCursor .cur/.ani/.ico decoding, palette, bottom-up rows, AND mask, hotspot, refusal:" + (real.isEmpty() ? "" : real) + " PASS");
    }
}
