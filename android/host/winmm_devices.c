/* These legacy imports report that the optional Windows devices are absent.
 * The games and Bink select the native Miles PCM driver. */
#include "host.h"

#define WINMM "winmm.dll"
WINAPI_EXPORT(WINMM, waveOutGetNumDevs, 0) { return 0; }
WINAPI_EXPORT(WINMM, waveOutOpen, 6) { return 6; }
WINAPI_EXPORT(WINMM, waveOutClose, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveOutGetID, 2) { return 5; }
WINAPI_EXPORT(WINMM, waveOutGetDevCapsA, 3) { return 2; }
WINAPI_EXPORT(WINMM, waveOutPrepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveOutUnprepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveOutWrite, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveOutReset, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveOutPause, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveOutRestart, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveOutSetVolume, 2) { return 5; }

/* No MIDI, recording, CD-audio or system mixer device exists on this port. */
WINAPI_EXPORT(WINMM, midiOutOpen, 5) { if (ARG(0)) MEM32(ARG(0)) = 0; return 6; }
WINAPI_EXPORT(WINMM, midiOutLongMsg, 3) { return 5; }
WINAPI_EXPORT(WINMM, midiOutShortMsg, 2) { return 5; }
WINAPI_EXPORT(WINMM, midiOutClose, 1) { return 5; }
WINAPI_EXPORT(WINMM, midiOutReset, 1) { return 5; }
WINAPI_EXPORT(WINMM, midiOutUnprepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, midiOutPrepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, auxGetDevCapsA, 3) { return 2; }
WINAPI_EXPORT(WINMM, auxGetNumDevs, 0) { return 0; }
WINAPI_EXPORT(WINMM, auxGetVolume, 2) { return 2; }
WINAPI_EXPORT(WINMM, auxSetVolume, 2) { return 2; }
WINAPI_EXPORT(WINMM, mixerClose, 1) { return 5; }
WINAPI_EXPORT(WINMM, mixerGetControlDetailsA, 3) { return 5; }
WINAPI_EXPORT(WINMM, mixerGetLineInfoA, 3) { return 5; }
WINAPI_EXPORT(WINMM, mixerOpen, 5) { if (ARG(0)) MEM32(ARG(0)) = 0; return 6; }
WINAPI_EXPORT(WINMM, mixerGetLineControlsA, 3) { return 5; }
WINAPI_EXPORT(WINMM, mixerGetNumDevs, 0) { return 0; }
WINAPI_EXPORT(WINMM, mixerSetControlDetails, 3) { return 5; }
WINAPI_EXPORT(WINMM, mciSendCommandA, 4) { return 272; } /* MCIERR_DEVICE_NOT_INSTALLED */
WINAPI_EXPORT(WINMM, waveInAddBuffer, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveInPrepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveInOpen, 6) { if (ARG(0)) MEM32(ARG(0)) = 0; return 6; }
WINAPI_EXPORT(WINMM, waveInClose, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveInUnprepareHeader, 3) { return 5; }
WINAPI_EXPORT(WINMM, waveInReset, 1) { return 5; }
WINAPI_EXPORT(WINMM, waveInStart, 1) { return 5; }
