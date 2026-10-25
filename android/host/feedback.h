/* Haptic cues and frame timing shared by the native modules and the Java layer. */
#ifndef STRONGHOLD_FEEDBACK_H
#define STRONGHOLD_FEEDBACK_H

/* Events the Android layer turns into haptic ticks; any thread may raise them. */
enum { SH_FEEDBACK_ZOOM_SNAP = 1, SH_FEEDBACK_SECTION = 2, SH_FEEDBACK_PLACE = 4 };
void stronghold_feedback(int bits);
/* Returns the raised events and clears them (JNI, UI thread). */
int stronghold_feedback_take(void);

/* Guest-thread frame seams: begin at the start of a game frame, end once its
 * picture is submitted, before the swap may wait for the display. */
void stronghold_perf_frame_begin(void);
void stronghold_perf_frame_end(void);
/* Recent frames: [0] mean busy ms, [1] 95th percentile busy ms, [2] frames
 * per second, [3] frames sampled. Returns 0 until enough frames exist. */
int stronghold_perf_stats(float stats[4]);
void stronghold_perf_reset(void);

#endif
