package port.launcher;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

/** Stand-in for the launcher base's Vulkan driver probe: this 2D port draws with OpenGL ES only,
 *  so the shared SystemInfo finds no probe report and takes the GPU strings from the game. */
final class Probe {
    static final String REPORT = "probe.txt";

    private Probe() { }

    static String read(File file) { return null; }

    static Map<String, String> parse(String report) { return new HashMap<>(); }
}
