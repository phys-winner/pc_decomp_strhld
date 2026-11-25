package port.launcher;

import java.io.EOFException;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class ArchiveMd5 {
    private static final String TOKEN = "([0-9a-f]{32})(?![a-z0-9])";
    private static final Pattern TAGGED = Pattern.compile("(?<![a-z0-9])md5[ ._=-]*" + TOKEN, Pattern.CASE_INSENSITIVE);
    private static final Pattern BARE = Pattern.compile("(?<![a-z0-9])" + TOKEN, Pattern.CASE_INSENSITIVE);

    interface Progress { void update(long done, long total); }

    static String expected(String name) {
        if (name == null) return null;
        Matcher tagged = TAGGED.matcher(name);
        if (tagged.find()) return tagged.group(1);
        Matcher bare = BARE.matcher(name);
        return bare.find() ? bare.group(1) : null;
    }

    static boolean matches(FileChannel file, String expected, Progress progress) throws IOException {
        MessageDigest digest;
        try { digest = MessageDigest.getInstance("MD5"); }
        catch (NoSuchAlgorithmException e) { throw new IOException(e); }
        ByteBuffer buffer = ByteBuffer.allocate(256 * 1024);
        long size = file.size(), done = 0;
        progress.update(0, size);
        while (done < size) {
            if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException();
            buffer.clear();
            buffer.limit((int)Math.min(buffer.capacity(), size - done));
            int count = file.read(buffer, done);
            if (count <= 0) throw new EOFException();
            digest.update(buffer.array(), 0, count);
            done += count;
            progress.update(done, size);
        }
        if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException();
        if (file.size() != size) throw new EOFException();
        byte[] actual = digest.digest();
        for (int i = 0; i < actual.length; ++i)
            if ((actual[i] & 255) != Integer.parseInt(expected.substring(2 * i, 2 * i + 2), 16)) return false;
        return true;
    }
}
