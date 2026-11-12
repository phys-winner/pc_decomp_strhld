"""Real stream counters report progress within large files and retain cancellation."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "android/app/src/org/stronghold/port/LauncherActivity.java").read_text(encoding="utf-8")
methods = []
for name in ("private static final class Counting", "private void copyStream("):
    found = re.search(r"^    " + re.escape(name) + r"[^\n]*\{.*?^    }", source, re.M | re.S)
    assert found, name
    methods.append(found.group())
program = r'''
import java.io.*;
import java.nio.file.*;
import java.util.*;
public class TransferProgress {
    boolean transferCancelled;
''' + "\n".join(methods) + r'''
    public static void main(String[] args) throws Exception {
        byte[] data=new byte[700000];new Random(5).nextBytes(data);
        List<Long> counted=new ArrayList<>();
        Counting input=new Counting(new ByteArrayInputStream(data),counted::add);
        assert input.read()==(data[0]&255);
        byte[] buffer=new byte[17];
        assert input.read(buffer)==17&&input.count==18;
        while(input.read(buffer,0,buffer.length)!=-1){}
        assert input.count==data.length&&counted.get(counted.size()-1)==data.length;
        File output=new File(args[0],"copied.bin");
        TransferProgress worker=new TransferProgress();List<Long> progress=new ArrayList<>();
        worker.copyStream(new ByteArrayInputStream(data),output,bytes->{
            assert output.length()==bytes;progress.add(bytes);
        });
        assert progress.size()>1&&progress.get(0)<data.length&&progress.get(progress.size()-1)==data.length;
        assert Arrays.equals(data,Files.readAllBytes(output.toPath()));
        try {
            worker.copyStream(new ByteArrayInputStream(data),output,bytes->worker.transferCancelled=true);
            throw new AssertionError("cancellation ignored");
        } catch(InterruptedIOException expected){assert output.length()>0&&output.length()<data.length;}
        System.out.println("PASS transfer progress: mixed reads, exact counters, in-file updates, output integrity, cancellation");
    }
}
'''
with tempfile.TemporaryDirectory(prefix="stronghold-progress-") as temporary:
    folder = Path(temporary)
    path = folder / "TransferProgress.java"
    path.write_text(program, encoding="utf-8")
    java = Path(os.environ["JAVA_HOME"]) / "bin"
    subprocess.run(list(map(str, [java / "javac.exe", "--release", "8", "-d", folder, path])), check=True)
    subprocess.run(list(map(str, [java / "java.exe", "-ea", "-cp", folder, "TransferProgress", folder])), check=True)
