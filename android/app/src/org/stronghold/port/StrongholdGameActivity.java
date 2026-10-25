package org.stronghold.port;

/** The original Stronghold HD in its own process, library (stronghold_sh) and profile. */
public final class StrongholdGameActivity extends GameActivity {
    @Override protected String gameEdition() { return "stronghold"; }
}
