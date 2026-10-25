package org.stronghold.port;

/** A separate Android process owns the Extreme library and profile. */
public final class ExtremeGameActivity extends GameActivity {
    @Override protected String gameEdition() { return "extreme"; }
}
