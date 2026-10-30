#ifndef M8_MIDI_WIN_H
#define M8_MIDI_WIN_H
#include "worker.h"
/* External USB/MIDI devices routed to and from the M8 (Windows, WinMM).
 * Filters: NULL or "" = all devices, "none" = off, else a name substring. */
void m8_midi_start(m8_worker *worker, const char *input_filter, const char *output_filter);
void m8_midi_poll(void);   /* main thread, often: send M8 output, notice hotplug */
void m8_midi_stop(void);   /* before the worker stops */
#endif
