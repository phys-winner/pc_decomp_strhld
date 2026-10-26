#ifndef STRONGHOLD_VIEW_CONTROLS_H
#define STRONGHOLD_VIEW_CONTROLS_H
#include <math.h>

/* The same gesture gain and 100% detent in every edition. */
#define SH_RADAR_PAN_GAIN 8.f
static inline double stronghold_zoom_step(double old, double *unsnapped, int *snap, float factor)
{
    if (factor == 1.f) return old;
    const double low = .94, high = 1.06;
    *unsnapped = fmax(.5, fmin(2.5, *unsnapped * factor));
    int inside = *unsnapped >= low && *unsnapped <= high;
    if (!*snap && inside) *snap = 1;
    if (*snap == 1) {
        if (inside) return 1;
        *unsnapped /= *unsnapped > 1 ? high : low;
        *snap = 2;
    }
    return *unsnapped;
}
#endif
