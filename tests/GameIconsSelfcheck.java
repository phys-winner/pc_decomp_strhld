package org.stronghold.port;

import java.io.File;

public final class GameIconsSelfcheck {
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) {
        byte[] stream = {0, 0, 124, 32, 64, 31, 0, (byte)128};
        GameIcons icon = GameIcons.decode(stream, 3, 1, false);
        check(icon != null && icon.argb[0] == 0xffff0000 && icon.argb[1] == 0 && icon.argb[2] == 0xff0000ff);
        check(GameIcons.decode(new byte[]{0, 1}, 1, 1, false) == null);
        check(GameIcons.decode(new byte[]{65, 0, 124}, 1, 1, false) == null);
        check(GameIcons.decode(new byte[]{96}, 1, 1, false) == null);
        check(GameIcons.decode(stream, 1024, 1024, false) == null);
        check(GameIcons.decode(new byte[]{(byte)128, 1}, 1, 1, false) == null);
        check(GameIcons.decode(new byte[]{0, 0, 124, (byte)128, (byte)128}, 1, 1, false) != null);
        for (String path : args) for (String id : ControlLayout.IDS) if (GameIcons.file(id) != null) {
            GameIcons original = GameIcons.load(new File(path), id);
            if (original == null) throw new AssertionError("Unreadable original sprite: " + id + " in " + path);
            check(original.width > 0 && original.height > 0 && original.mirrored == "rotate_right".equals(id));
            boolean visible = false;
            for (int pixel : original.argb) visible |= (pixel >>> 24) != 0;
            check(visible);
        }
        System.out.println("Game icons: literal/repeat/transparent RGB555, malformed input, bounds and original sprites PASS");
    }
}
