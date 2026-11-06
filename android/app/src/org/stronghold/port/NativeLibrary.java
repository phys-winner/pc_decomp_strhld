package org.stronghold.port;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;
import java.io.*;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/** The launcher unpacks only the selected edition, on its existing preflight worker. */
final class NativeLibrary {
    private static native long nativeUnpack(AssetManager assets, String name, String path,
                                            long total, int chunk, int[] sizes);

    private NativeLibrary() { }

    static synchronized File prepare(Context context, String library) throws IOException {
        String name = "lib" + library + ".so";
        if (!library.equals("stronghold") && !library.equals("stronghold_classic") && !library.equals("stronghold_sh"))
            throw new IOException("Unknown native library");
        String[] fields = null;
        try (InputStream in = context.getAssets().open("native/index.txt")) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            for (int n; (n = in.read(buffer)) != -1;) {
                if (bytes.size() + n > 4096) throw new IOException("Native index too large");
                bytes.write(buffer, 0, n);
            }
            for (String line : new String(bytes.toByteArray(), StandardCharsets.US_ASCII).split("\n")) {
                String[] row = line.trim().split("\\s+");
                if (row.length > 0 && name.equals(row[0])) {
                    if (fields != null) throw new IOException("Duplicate native library");
                    fields = row;
                }
            }
        }
        long total;
        int chunk;
        int[] sizes;
        try {
            if (fields == null || fields.length != 5 || !fields[2].matches("[0-9a-f]{64}"))
                throw new IllegalArgumentException();
            total = Long.parseLong(fields[1]);
            chunk = Integer.parseInt(fields[3]);
            String[] packed = fields[4].split(",", -1);
            if (total <= 0 || total > (1L << 30) || chunk < (1 << 20) || chunk > (32 << 20)
                    || packed.length > 64 || (total + chunk - 1) / chunk != packed.length)
                throw new IllegalArgumentException();
            sizes = new int[packed.length];
            for (int i = 0; i < sizes.length; i++) {
                sizes[i] = Integer.parseInt(packed[i]);
                if (sizes[i] <= 0) throw new IllegalArgumentException();
            }
        } catch (IllegalArgumentException invalid) {
            throw new IOException("Invalid native library index", invalid);
        }
        File dir = new File(context.getFilesDir(), "native");
        if (!dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) throw new IOException("Cannot create native directory");
        File target = new File(dir, name), stamp = new File(dir, name + ".sha256");
        try (RandomAccessFile lock = new RandomAccessFile(new File(dir, name + ".lock"), "rw");
             FileLock held = lock.getChannel().lock()) {
            if (target.isFile() && target.length() == total && stamp.isFile() && stamp.length() == 65
                    && fields[2].equals(new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.US_ASCII).trim())
                    && fields[2].equals(GameFiles.sha256(target))) return target;
            File part = new File(dir, name + ".part"), stampPart = new File(dir, name + ".sha256.part");
            Files.deleteIfExists(part.toPath());
            Files.deleteIfExists(stampPart.toPath());
            if (dir.getUsableSpace() < total + (8 << 20)) throw new IOException("Not enough free space to unpack game library");
            System.loadLibrary("stronghold_xz");
            long started = android.os.SystemClock.elapsedRealtime();
            try {
                long written = nativeUnpack(context.getAssets(), "native/" + name + ".xz", part.getAbsolutePath(), total, chunk, sizes);
                if (written != total || part.length() != total || !fields[2].equals(GameFiles.sha256(part)))
                    throw new IOException("Native library unpack/verification failed (" + written + ")");
                if (!part.setReadOnly()) throw new IOException("Cannot protect native library");
                Files.move(part.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                try (FileOutputStream out = new FileOutputStream(stampPart)) {
                    out.write((fields[2] + "\n").getBytes(StandardCharsets.US_ASCII));
                    out.getFD().sync();
                }
                Files.move(stampPart.toPath(), stamp.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } finally {
                Files.deleteIfExists(part.toPath());
                Files.deleteIfExists(stampPart.toPath());
            }
            Log.i("StrongholdNative", "Unpacked " + name + " bytes=" + total + " ms="
                    + (android.os.SystemClock.elapsedRealtime() - started) + " sha256=" + fields[2]);
            return target;
        }
    }

    static void load(Context context, String library) throws IOException {
        System.load(prepare(context, library).getAbsolutePath());
    }
}
