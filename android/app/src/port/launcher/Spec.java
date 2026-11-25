package port.launcher;

import android.content.Context;
import java.util.ArrayList;
import java.util.List;

/** Stand-in for the launcher base's port.json specification. Stronghold keeps its own launcher
 *  shell (three editions in three processes, its importer, UCP and mods), so only the facts the
 *  shared FPS overlay and game-mode tip read are given here: one OpenGL ES renderer, the FPS
 *  counter option (off on a fresh install) and the tip turned on. */
final class Spec {
    static final class Mode {
        String id = "gles", kind = "gles", title = "OpenGL ES";
    }

    static final class Option {
        String key, value;
        Option(String key, String value) { this.key = key; this.value = value; }
    }

    static final String FPS_COUNTER = "fps_counter";
    private static final Spec SPEC = new Spec();
    /** "OpenGL ES 3.2" once the game's context reported its version (Shared.overlay). */
    static volatile String api = "OpenGL ES";

    final boolean hyperosNotice = true;
    final String defaultMode = "gles";
    final List<Option> options = new ArrayList<>();

    private Spec() { options.add(new Option(FPS_COUNTER, "0")); }

    static Spec get(Context context) { return SPEC; }

    Mode mode(String id) {
        Mode mode = new Mode();
        mode.title = api;
        return mode;
    }
}
