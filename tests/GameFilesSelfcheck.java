package org.stronghold.port;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public final class GameFilesSelfcheck {
    public static void main(String[] args) throws Exception {
        Path fixture = Files.createTempDirectory("stronghold-launcher-");
        assert GameFiles.lastSuccess(fixture.toFile()) == null;
        String directory = fixture.resolve("Папка игры").toString();
        GameFiles.rememberSuccess(fixture.toFile(), "classic", directory);
        assert java.util.Arrays.equals(GameFiles.lastSuccess(fixture.toFile()), new String[] {"classic", directory});
        GameFiles.rememberSuccess(fixture.toFile(), "stronghold", directory);
        assert GameFiles.lastSuccess(fixture.toFile())[0].equals("stronghold");
        Files.write(fixture.resolve("last-game"), new byte[] {0, 9, 1});
        assert GameFiles.lastSuccess(fixture.toFile()) == null : "A partial/corrupt record must open the launcher";
        GameFiles.rememberSuccess(fixture.toFile(), "invalid", directory);
        assert GameFiles.lastSuccess(fixture.toFile()) == null;
        GameFiles.rememberSuccess(fixture.toFile(), "classic", "relative");
        assert GameFiles.lastSuccess(fixture.toFile()) == null;
        File collision = fixture.resolve("duplicate.map").toFile();
        Files.write(collision.toPath(), new byte[] {1});
        assert GameFiles.uniqueFile(collision).getName().equals("duplicate (2).map");
        assert Files.readAllBytes(collision.toPath())[0] == 1;
        Files.write(GameFiles.uniqueFile(collision).toPath(), new byte[] {2});
        assert GameFiles.uniqueFile(collision).getName().equals("duplicate (3).map");
        File rejectedByAndroid = new File(fixture.toFile(), "maps/8 ??????? ? ?????? ????..map");
        File stage = GameFiles.archiveStaging(rejectedByAndroid, 37);
        assert stage.getParentFile().equals(rejectedByAndroid.getParentFile());
        assert stage.getName().equals(".stronghold-import-37.tmp") && !stage.getName().contains("?");
        assert GameFiles.archiveStaging(fixture.resolve("clean.map").toFile(), 38).getName().equals("clean.map");
        assert GameFiles.archiveMapName("wrapped/maps/????????.map", 0xaf0c6966L)
                .equals("wrapped/maps/Карта AF0C6966.map");
        assert GameFiles.archiveMapName("maps/mission1.map", 1).equals("maps/mission1.map");
        assert GameFiles.archiveMapName("maps/Ущелья.map", 1).equals("maps/Ущелья.map");
        assert GameFiles.archiveMapName("gm/???.wav", 1).equals("gm/???.wav");
        File generic = new File(fixture.toFile(), GameFiles.archiveMapName("????????.map", 1));
        Files.write(generic.toPath(), new byte[] {1});
        assert GameFiles.uniqueFile(generic).getName().equals("Карта 00000001 (2).map");
        File game = fixture.resolve("game").toFile();
        assert game.mkdir();
        assert GameFiles.validate(new File("relative"), "classic", false) != null;
        assert GameFiles.validate(game, "classic", false).contains("aiv");
        for (String name : new String[] {"aiv", "fx", "gfx", "gm", "maps"}) assert new File(game, name).mkdir();
        Files.write(new File(game, "cr.tex").toPath(), new byte[] {1});
        assert GameFiles.validate(game, "extreme", false).contains("mapsExtreme");
        assert new File(game, "mapsExtreme").mkdir();
        Path lifted = fixture.resolve("lifted"); Files.createDirectory(lifted);
        for (String name : new String[] {"Stronghold Crusader.exe", "Stronghold_Crusader_Extreme.exe", "Mss32.dll", "binkw32.dll"})
            Files.copy(new File(args[0], name).toPath(), lifted.resolve(name));
        for (String edition : new String[] {"classic", "extreme"}) {
            assert GameFiles.images(game, edition).equals(lifted.toFile());
            assert GameFiles.validate(game, edition, true) == null;
        }
        for (File file : lifted.toFile().listFiles()) Files.copy(file.toPath(), game.toPath().resolve(file.getName()));
        assert GameFiles.images(game, "classic").equals(game);
        assert GameFiles.validate(game, "classic", true) == null;
        Files.write(new File(game, "Mss32.dll").toPath(), new byte[] {1, 2, 3});
        assert GameFiles.validate(game, "classic", true).contains("Mss32.dll");
        Files.delete(new File(game, "Mss32.dll").toPath());
        assert GameFiles.validate(game, "classic", false).contains("Не найден");
        assert !GameFiles.pending(fixture.toFile(), "classic").equals(GameFiles.pending(fixture.toFile(), "extreme"));
        /* The original Stronghold HD: its own folder, assets, PE copies, library and profile. */
        File original = fixture.resolve("stronghold-hd").toFile();
        assert original.mkdir();
        for (String name : new String[] {"fx", "gfx", "gm"}) assert new File(original, name).mkdir();
        assert GameFiles.validate(original, "stronghold", false).contains("maps");
        assert new File(original, "maps").mkdir();
        assert GameFiles.validate(original, "stronghold", false).contains("sh.tex");
        Files.write(new File(original, "sh.tex").toPath(), new byte[] {1});
        Path liftedOriginal = fixture.resolve("lifted-stronghold"); Files.createDirectory(liftedOriginal);
        for (String name : new String[] {"Stronghold.exe", "Mss32.dll", "binkw32.dll"})
            Files.copy(new File(args[0], name).toPath(), liftedOriginal.resolve(name));
        assert GameFiles.images(original, "stronghold").equals(liftedOriginal.toFile());
        assert GameFiles.validate(original, "stronghold", true) == null;
        assert GameFiles.validate(game, "stronghold", false) != null : "Crusader assets are not the original";
        Files.write(liftedOriginal.resolve("Stronghold.exe"), new byte[] {1});
        assert GameFiles.validate(original, "stronghold", true).contains("Stronghold.exe");
        assert GameFiles.library("stronghold").equals("stronghold_sh") && GameFiles.userdata("stronghold").equals("userdata-stronghold");
        assert GameFiles.library("classic").equals("stronghold_classic") && GameFiles.library("extreme").equals("stronghold");
        assert GameFiles.userdata("classic").equals("userdata-classic") && GameFiles.userdata("extreme").equals("userdata");
        assert !GameFiles.pending(fixture.toFile(), "stronghold").equals(GameFiles.pending(fixture.toFile(), "extreme"));
        try (java.util.stream.Stream<Path> paths = Files.walk(fixture)) {
            paths.sorted(java.util.Comparator.reverseOrder()).forEach(path -> { try { Files.delete(path); } catch (Exception error) { throw new RuntimeException(error); } });
        }
        System.out.println("GameFiles: atomic successful-start record, collision-safe map recovery, paths, assets, edition separation, SHA-256 rejection PASS");
    }
}
