package org.stronghold.port;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;

public final class GogInstallerSelfcheck {
    private static void rejected(String... lines) throws Exception {
        try { GogInstaller.parse(new File("setup.exe"), Arrays.asList(lines)); throw new AssertionError("Invalid installer metadata accepted"); }
        catch (IOException expected) { }
    }
    public static void main(String[] args) throws Exception {
        String valid="GOG1\t1207658713\t3903\t802416461\t3887\t3887\t0";
        GogInstaller.Info info=GogInstaller.parse(new File("setup.exe"),Arrays.asList("optional diagnostic",valid,"ROOT\tcr.tex","ROOT\tStronghold Crusader.exe"));
        assert info.game.equals("crusader") && !info.split && info.files==3903 && info.compressed==3887;
        rejected(valid); rejected(valid,valid,"ROOT\tcr.tex");
        rejected(valid,"ROOT\t../cr.tex");rejected(valid,"ROOT\tbad\u0000file");rejected(valid,"ROOT\tC:cr.tex");
        rejected(valid.replace("802416461","-1"),"ROOT\tcr.tex");
        rejected(valid.replace("1207658713","99"),"ROOT\tcr.tex");
        rejected(valid.replace("\t0","\t2"),"ROOT\tcr.tex");
        rejected(valid.replace("3887\t3887","3887\t4000"),"ROOT\tcr.tex");
        System.out.println("GOG metadata: real counts, bounds, malformed/duplicate headers, unknown IDs and unsafe paths PASS");
    }
}
