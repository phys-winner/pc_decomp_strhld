package org.stronghold.port;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collections;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class GameModsSelfcheck {
    private static byte[] archive(String... entries) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            for (int i=0; i<entries.length; i+=2) {
                zip.putNextEntry(new ZipEntry(entries[i])); zip.write(entries[i+1].getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
            }
        }
        return bytes.toByteArray();
    }
    private static void rejected(Path storage, String... entries) throws Exception {
        try { GameMods.install(new ByteArrayInputStream(archive(entries)), storage.toFile(), () -> false); throw new AssertionError("Accepted invalid ZIP"); }
        catch (IOException expected) { }
        assert storage.toFile().list().length == 1 : "Failed imports must leave no staging files";
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("stronghold-mod-check-"); Path storage = Files.createDirectory(root.resolve("mods"));
        String properties = "id=font-pack\nname=Localisation\nedition=crusader\n";
        byte[] input = archive("mod.properties", properties, "files/", "", "files/gfx/", "", "files/gfx/menu.gm1", "asset", "files/cr.tex", "text");
        GameMods.Pack pack = GameMods.install(new ByteArrayInputStream(input), storage.toFile(), () -> false);
        assert pack.supports("classic") && pack.supports("extreme") && !pack.supports("stronghold");
        assert GameMods.packs(storage.toFile(), "classic").size() == 1;
        assert GameMods.packs(storage.toFile(), "stronghold").isEmpty();
        assert Arrays.equals(Files.readAllBytes(pack.assets().toPath().resolve("cr.tex")), "text".getBytes(StandardCharsets.UTF_8));
        rejected(storage, "mod.properties", properties, "files/../../outside", "bad");
        rejected(storage, "mod.properties", properties, "files/Stronghold.exe", "bad");
        rejected(storage, "mod.properties", properties, "files/gfx/patch.dll", "bad");
        rejected(storage, "mod.properties", properties, "files/cr.tex", "one", "files/CR.TEX", "two");
        rejected(storage, "mod.properties", properties, "files/", ""); // Duplicate ID cannot replace the existing pack.
        rejected(storage, "mod.properties", "id=../escape\nname=Bad\nedition=classic", "files/gfx/test", "bad");
        try { GameMods.install(new ByteArrayInputStream(input), storage.toFile(), () -> true); throw new AssertionError(); }
        catch (java.io.InterruptedIOException expected) { }
        Path config = root.resolve("mods.cfg"); String pin = "fd326a41d8927eaee42dfb542658843138c15867";
        GameMods.configure(config.toFile(), pin, Collections.singleton("o_healer"), Collections.emptyList(), Collections.emptySet());
        byte[] before = Files.readAllBytes(config);
        assert new String(before, StandardCharsets.UTF_8).equals("UCP1 "+pin+"\no_healer=1\n");
        try { GameMods.configure(config.toFile(), pin, Collections.singleton("bad\n@/tmp"), Collections.emptyList(), Collections.emptySet()); throw new AssertionError(); }
        catch (IOException expected) { }
        assert Arrays.equals(before, Files.readAllBytes(config)) : "A failed save must retain the previous configuration";
        GameMods.configure(config.toFile(), pin, Collections.singleton("o_healer"), Collections.singletonList(pack), Collections.singleton(pack.id));
        GameMods.configureFeatures(config.toFile(), pin, Collections.emptySet());
        assert GameMods.features(config.toFile()).isEmpty();
        assert new String(Files.readAllBytes(config), StandardCharsets.UTF_8).equals("UCP1 "+pin+"\n@"+pack.assets().getCanonicalPath()+"\n")
                : "Disabling UCP must preserve the active localization";
        GameMods.configureFeatures(config.toFile(), pin, Collections.singleton("o_keys"));
        assert GameMods.features(config.toFile()).equals(Collections.singleton("o_keys"));
        before=Files.readAllBytes(config);
        try { GameMods.configureFeatures(config.toFile(), "0000000000000000000000000000000000000000", Collections.emptySet()); throw new AssertionError(); }
        catch (IOException expected) { }
        assert Arrays.equals(before, Files.readAllBytes(config)) : "A mismatched revision must leave the configuration intact";
        Path unrelated = Files.createDirectory(root.resolve("unrelated"));
        try { GameMods.delete(storage.toFile(), unrelated.toFile()); throw new AssertionError(); } catch (IOException expected) { }
        GameMods.delete(storage.toFile(), pack.folder);
        assert storage.toFile().list().length == 0 && Files.isDirectory(unrelated);
        System.out.println("GameMods: import, edition filtering, traversal/code/duplicate rejection, cancellation, atomic configuration and bounded deletion PASS");
    }
}
