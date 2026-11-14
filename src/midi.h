/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MIDI in from USB controllers, read straight from ALSA's raw MIDI device
 * files (/dev/snd/midiC*D*): no libasound to ship. Non-blocking: call
 * midi_poll from the UI loop. Devices plugged in later are found (it looks
 * again every two seconds), and ones pulled out are dropped. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t ch;   /* 0..15 */
    uint8_t note; /* 0: a control change (num = CC), 1: a note (num = note; val = velocity, 0 = off) */
    uint8_t num, val;
} MidiEv;

/* the events read since the last call, up to max */
int midi_poll(MidiEv *out, int max);
/* how many devices are open, and the first one's name ("" if none) */
int midi_count(void);
const char *midi_name(void);
void midi_close(void);
