package org.stronghold.port;

/** Empty touch input still receives events without submitting transparent Android frames. */
final class PointerRedraw {
    private boolean wasVisible;
    boolean needed(boolean visible) {
        boolean redraw = visible || wasVisible;
        wasVisible = visible;
        return redraw;
    }
}
