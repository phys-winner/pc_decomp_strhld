package org.stronghold.port;

/** A separate Android process owns the Classic library and profile. */
public final class ClassicGameActivity extends GameActivity {
    @Override protected String gameEdition() { return "classic"; }
}
