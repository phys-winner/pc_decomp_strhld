package org.stronghold.port;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Host check with file-backed Android stubs; the actual decoder has separate XZ checks. */
public final class NativeLibrarySelfcheck {
    public static void main(String[] args) throws Exception {
        File root = new File(args[0]), assets = new File(root, "assets"), files = new File(root, "files");
        File index = new File(assets, "native/index.txt"), target = new File(files, "native/libstronghold.so");
        index.getParentFile().mkdirs(); target.getParentFile().mkdirs();
        byte[] data = "complete native payload fixture".getBytes(StandardCharsets.US_ASCII);
        Files.write(target.toPath(), data);
        String hash = GameFiles.sha256(target);
        File stamp = new File(target.getParentFile(), target.getName() + ".sha256");
        Files.write(stamp.toPath(), (hash + "\n").getBytes(StandardCharsets.US_ASCII));
        String valid = "libstronghold.so " + data.length + " " + hash + " 1048576 16\n";
        Context context = new Context(assets, files);
        write(index, valid);
        assert NativeLibrary.prepare(context, "stronghold").equals(target) : "Verified cache must be reused";
        File part = new File(target.getParentFile(), target.getName() + ".part");
        write(part, "interrupted unpack");
        byte[] corrupt = data.clone(); corrupt[0] ^= 1; Files.write(target.toPath(), corrupt);
        try {
            NativeLibrary.prepare(context, "stronghold");
            throw new AssertionError("Same-sized corruption was accepted");
        } catch (UnsatisfiedLinkError expected) { /* A host has no Android decoder: a cache miss reached unpack. */ }
        assert !part.exists() : "Interrupted output must be removed before retry";
        Files.write(target.toPath(), data);
        write(stamp, "0" + hash.substring(1) + "\n");
        if (hash.charAt(0) == '0') write(stamp, "1" + hash.substring(1) + "\n");
        try {
            NativeLibrary.prepare(context, "stronghold");
            throw new AssertionError("Previous build stamp was accepted");
        } catch (UnsatisfiedLinkError expected) { }
        for (String bad : new String[] {"", valid + valid, valid.replace(hash, "invalid"),
                valid.replace(" 1048576 ", " 0 "), valid.replace(" 1048576 ", " 67108864 "),
                valid.replace(" 16\n", " 0\n"), valid.replace(" 16\n", " 1,2\n"),
                valid.replace(" " + data.length + " ", " 1073741825 "),
                valid.replace(" " + data.length + " ", " -1 "),
                valid.replace(" 16\n", " 2147483648\n"), new String(new char[4097]).replace('\0', 'x')}) {
            write(index, bad);
            try {
                NativeLibrary.prepare(context, "stronghold");
                throw new AssertionError("Malformed native index accepted");
            } catch (IOException expected) { }
        }
        try { NativeLibrary.prepare(context, "../other"); throw new AssertionError("Library path accepted"); }
        catch (IOException expected) { }
        System.out.println("NativeLibrary: cache/hash/build stamp, interrupted retry, malformed indices and path checks PASS");
    }

    private static void write(File path, String text) throws IOException {
        Files.write(path.toPath(), text.getBytes(StandardCharsets.US_ASCII));
    }
}
