package org.stronghold.port;

import java.io.File;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;

public final class GameImagesSelfcheck {
    public static void main(String[] args) throws Exception {
        Path input=new File(args[0]).toPath(), patches=new File(args[1]).toPath();
        int checked=0;
        for (GameCompatibility.Executable entry:GameCompatibility.EXECUTABLES) {
            if(entry.sha256.equals(entry.target)) continue;
            File source;
            try(java.util.stream.Stream<Path> paths=Files.walk(input)) {
                source=paths.filter(Files::isRegularFile).filter(p->p.getFileName().toString().equals(GameFiles.executable(entry.edition)))
                        .map(Path::toFile).filter(f->{try{return GameFiles.sha256(f).equals(entry.sha256);}catch(Exception error){throw new RuntimeException(error);}})
                        .findFirst().orElseThrow(()->new AssertionError("Missing owned EXE fixture"));
            }
            Path output=Files.createTempFile("stronghold-normalized-", ".exe");
            try (InputStream patch=Files.newInputStream(patches.resolve(entry.sha256+".delta"))) {
                GameImages.normalize(source,patch,output.toFile(),entry);
                assert Files.size(output)==entry.targetBytes && GameFiles.sha256(output.toFile()).equals(entry.target);
                assert GameFiles.sha256(source).equals(entry.sha256) : "Original EXE changed";
            } finally { Files.delete(output); }
            checked++;
        }
        assert checked==9;
        System.out.println("GameImages: all "+checked+" localized EXE deltas reproduce the exact canonical engines; original inputs unchanged PASS");
    }
}
