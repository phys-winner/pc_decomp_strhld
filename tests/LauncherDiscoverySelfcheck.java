package org.stronghold.port;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Real supported PE files plus small asset fixtures exercise the shared launcher discovery. */
public final class LauncherDiscoverySelfcheck {
    private static File install(Path root, Path input, boolean original, boolean extreme) throws Exception {
        Files.createDirectories(root);
        for (String name : new String[] {"fx", "gfx", "gm", "maps"}) Files.createDirectory(root.resolve(name));
        Files.write(root.resolve(original ? "sh.tex" : "cr.tex"), new byte[] {1});
        if (!original) Files.createDirectory(root.resolve("aiv"));
        if (extreme) Files.createDirectory(root.resolve("mapsExtreme"));
        String executable = original ? "Stronghold.exe" : "Stronghold Crusader.exe";
        for (String name : new String[] {executable, "Mss32.dll", "binkw32.dll"}) Files.copy(input.resolve(name), root.resolve(name));
        if (extreme) Files.copy(input.resolve("Stronghold_Crusader_Extreme.exe"), root.resolve("Stronghold_Crusader_Extreme.exe"));
        return root.toFile();
    }
    private static void paths(GameFiles.Scan result, String... expected) {
        assert Arrays.equals(result.paths, expected) : Arrays.toString(result.paths) + " != " + Arrays.toString(expected);
    }
    private static void missing(GameFiles.Scan result) {
        assert !result.complete();
        for (String problem : result.problems) assert problem != null;
    }
    public static void main(String[] args) throws Exception {
        Path input = new File(args[0]).toPath(), root = Files.createTempDirectory("stronghold-discovery-");
        try {
            assert Arrays.equals(GameFiles.savedPaths(null, null, null),
                    new String[] {GameFiles.DEFAULT_GAME, GameFiles.DEFAULT_GAME, GameFiles.DEFAULT_STRONGHOLD});
            assert Arrays.equals(GameFiles.savedPaths(" /legacy/path ", null, " /original "),
                    new String[] {"/legacy/path", "/legacy/path", "/original"});
            assert GameFiles.savedPaths("/classic", "/extreme", "/original")[1].equals("/extreme");
            assert GameFiles.savedPaths("/classic", "", "")[1].isEmpty() : "An explicit empty path must be validated, not replaced by another edition";
            assert GameFiles.pathKey("classic").equals("game");
            assert GameFiles.pathKey("extreme").equals("extremeGame");
            assert GameFiles.pathKey("stronghold").equals("strongholdGame");

            String absent = root.resolve("not-installed").toString();
            String[] empty = {"", absent, absent};
            Path common = root.resolve("common");
            File crusader = install(common.resolve("wrapped/Crusader"), input, false, true);
            File stronghold = install(common.resolve("wrapped/Stronghold"), input, true, false);
            GameFiles.Scan discovered = GameFiles.scan(empty, common.toFile(), false, true);
            paths(discovered, crusader.toString(), crusader.toString(), stronghold.toString());
            assert discovered.complete() && discovered.discoveryProblem == null;
            assert empty[0].isEmpty() && empty[1].equals(absent) : "Discovery must not mutate the caller's saved paths";
            String[] working = discovered.paths.clone();
            assert GameFiles.scan(working, null, false, true).complete();

            String[] aliases = {new File(crusader, ".").getPath(),
                    new File(crusader.getParentFile(), "Crusader/../Crusader").getPath(), new File(stronghold, ".").getPath()};
            assert !aliases[0].equals(working[0]) && !aliases[1].equals(working[1]);
            GameFiles.Scan unchanged = GameFiles.scan(aliases, common.toFile(), true, true);
            paths(unchanged, aliases);
            assert unchanged.complete() : "Selecting the same real folders must preserve valid saved path strings for running games";
            paths(GameFiles.scan(empty, common.toFile(), true, true), working);

            File replacement = install(root.resolve("replacement"), input, false, true);
            GameFiles.Scan automatic = GameFiles.scan(new String[] {working[0], absent, working[2]}, replacement, false, true);
            paths(automatic, working[0], replacement.toString(), working[2]);
            assert automatic.complete() : "Automatic discovery fills only invalid paths";
            GameFiles.Scan selected = GameFiles.scan(working, replacement, true, true);
            paths(selected, replacement.toString(), replacement.toString(), working[2]);
            assert selected.complete() : "A shared Crusader folder satisfies both editions while preserving Stronghold";
            paths(GameFiles.scan(aliases, replacement, true, true), replacement.toString(), replacement.toString(), aliases[2]);

            File classicOnly = install(root.resolve("classic-only"), input, false, false);
            GameFiles.Scan classic = GameFiles.scan(working, classicOnly, true, true);
            paths(classic, classicOnly.toString(), working[1], working[2]);
            assert classic.complete() && classic.discoveryProblem == null;
            File extremeOnly = install(root.resolve("extreme-only"), input, false, true);
            Files.write(new File(extremeOnly, "Stronghold Crusader.exe").toPath(), new byte[] {1});
            GameFiles.Scan extreme = GameFiles.scan(working, extremeOnly, true, true);
            paths(extreme, working[0], extremeOnly.toString(), working[2]);
            assert extreme.complete() : "An unsupported Classic executable must not overwrite the working Classic path";

            File broken = install(root.resolve("broken"), input, false, true);
            Files.write(new File(broken, "Mss32.dll").toPath(), new byte[] {1, 2, 3});
            GameFiles.Scan rejected = GameFiles.scan(working, broken, true, true);
            paths(rejected, working);
            assert rejected.complete() && rejected.discoveryProblem.contains("Mss32.dll") : "Report the hash error and retain all valid paths";
            GameFiles.Scan absentResult = GameFiles.scan(empty, root.resolve("empty-folder").toFile(), true, true);
            missing(absentResult);
            assert absentResult.discoveryProblem != null;
            assert GameFiles.scan(new String[] {"", "", ""}, null, false, true).problems[0].contains("абсолютный путь");
            GameFiles.Scan relative = GameFiles.scan(working, new File("relative"), true, true);
            paths(relative, working);
            assert relative.discoveryProblem.contains("абсолютный путь");

            File deep = install(root.resolve("bounded/a/b/c/d"), input, true, false);
            GameFiles.Scan depthFour = GameFiles.scan(empty, root.resolve("bounded").toFile(), true, true);
            assert depthFour.paths[2].equals(deep.toString()) && depthFour.problems[2] == null;
            install(root.resolve("too-deep/a/b/c/d/e"), input, true, false);
            missing(GameFiles.scan(empty, root.resolve("too-deep").toFile(), true, true));

            Path bounded = Files.createDirectory(root.resolve("links"));
            boolean linked = false;
            try { Files.createSymbolicLink(bounded.resolve("outside"), common); linked = true; }
            catch (java.io.IOException | UnsupportedOperationException | SecurityException unavailable) {
                System.out.println("Discovery symlink boundary fixture unavailable: " + unavailable.getClass().getSimpleName());
            }
            if (linked) {
                missing(GameFiles.scan(empty, bounded.toFile(), true, true));
                Files.createSymbolicLink(bounded.resolve("cycle"), bounded);
                missing(GameFiles.scan(empty, bounded.toFile(), true, true));
            }
            System.out.println("Launcher discovery: legacy preference migration, all three editions, shared Crusader folder, valid path alias retention, independent path replacement, invalid/empty paths, SHA-256 rejection, bounded traversal PASS");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                paths.sorted(java.util.Comparator.reverseOrder()).forEach(path -> {
                    try { Files.delete(path); } catch (Exception error) { throw new RuntimeException(error); }
                });
            }
        }
    }
}
