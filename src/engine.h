/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TrimFX engine: a four-module stereo FX chain at 48 kHz.
 *
 *   in -> TONE (mastering / tonal shaping) -> MOD -> DELAY -> REVERB -> out
 *
 * (MOD, DELAY and REVERB can be put in any order; TONE always comes first.)
 * Every module has several algorithms, eight parameters per algorithm
 * (0..1 each, each algorithm keeps its own), a mix that goes to 100 % wet
 * (no dry at all: for an FX send / return), and a bypass. DELAY and REVERB
 * bypass with trails: their input closes and what is ringing rings out.
 *
 * Parameters come in as a whole Patch each block (the UI thread publishes
 * one with the sticks' movement already applied); the engine glides them. */
#pragma once
#include <stdint.h>

enum { M_TONE, M_MOD, M_DELAY, M_VERB, NMOD };
#define NPARAM 16  /* 0-7 on the board (0-3 on the sticks), 8-15 on the DEEP page */
#define NBOARD 8
#define MAXALGO 10
#define NORDER 6

typedef struct {
    int algo;
    int bypass;
    float mix;                    /* 0 dry .. 1 all wet */
    float p[MAXALGO][NPARAM];     /* each algorithm's own settings, 0..1 */
} ModState;

typedef struct {
    ModState m[NMOD];
    float in_db, out_db;          /* -24..+24 */
    float dry;                    /* the dry input added at the very end, 0..1 (0 for an FX send) */
    float bpm;                    /* tempo for the rhythmic delay, 40..240 */
    int order;                    /* 0..NORDER-1: the order of MOD, DELAY, REVERB */
    int freeze_dly, freeze_verb;  /* hold the delay's / reverb's contents, input closed */
} Patch;

/* what the UI shows about each module and algorithm */
typedef struct {
    const char *name;                       /* "TONE" */
    int nalgo;
    const char *algo[MAXALGO];              /* "LONDON" */
    const char *blurb[MAXALGO];             /* one line about the algorithm */
    const char *pname[MAXALGO][NPARAM];     /* "" = unused */
    float pdef[MAXALGO][NPARAM];
    float mixdef;
    /* the value as text, e.g. "2.4 s" */
    void (*fmt)(int algo, int p, float v, const Patch *pt, char *out, int n);
} ModInfo;

extern const ModInfo tone_info, modu_info, delay_info, verb_info;
const ModInfo *eng_info(int mod);
const char *eng_order_name(int order);      /* "MOD>DLY>VERB" */
void eng_order(int order, int out[3]);      /* the three modules after TONE */

/* stepped parameters: a 0..1 value as one of n steps */
static inline int pstep(float v, int n)
{
    int i = (int)(v * (float)n);
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

/* tempo sync for a rate: FREE (the free rate), or one cycle per bar / note value */
#define NSYNC 10
static inline float sync_hz(float raw, float bpm, float free_hz)
{
    static const float B[NSYNC] = {0.f, 16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f, 2.f / 3.f, 1.f / 3.f}; /* beats a cycle */
    int s = pstep(raw, NSYNC);
    return s ? (bpm > 1.f ? bpm : 120.f) / 60.f / B[s] : free_hz;
}
static inline const char *sync_name(float v)
{
    static const char *const N[NSYNC] = {"FREE", "4 BARS", "2 BARS", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/4 T", "1/8 T"};
    return N[pstep(v, NSYNC)];
}

void patch_default(Patch *p);
/* a factory preset: 0..NFACTORY-1 */
#define NFACTORY 8
const char *patch_factory(Patch *p, int i);

typedef struct Engine Engine;
Engine *engine_new(void);
void engine_free(Engine *e);
void engine_reset(Engine *e);
/* n frames of interleaved stereo in, out (may be the same) */
void engine_process(Engine *e, const Patch *p, const float *in, float *out, int n);

typedef struct {
    float in_pk[2], out_pk[2];     /* peaks since the last read, linear */
    float mod_pk[NMOD];            /* each module's output */
    float gr_db;                   /* TONE's gain reduction */
    int algo_now[NMOD];
} EngMeters;
/* read (and reset) the meters: from the UI thread, lock-free (approximate) */
void engine_meters(Engine *e, EngMeters *m);
