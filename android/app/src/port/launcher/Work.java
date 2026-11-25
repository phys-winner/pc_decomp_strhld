package port.launcher;

/** Stand-in for the launcher base's Work: the shared State only needs its warning log. */
final class Work {
    private Work() { }

    static void warn(String text, Throwable error) {
        try {
            android.util.Log.w("Launcher", text, error);
        } catch (RuntimeException stub) {
            System.err.println(text);
        }
    }
}
