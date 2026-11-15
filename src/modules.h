/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The four modules' insides, as the engine drives them. Each works on
 * separate left / right buffers, in place, at most MOD_BLOCK frames, and
 * gives only the wet signal (the engine does mix and bypass). p: the
 * playing algorithm's eight parameters (glided), raw: the same unglided
 * (for stepped choices). */
#pragma once
#include "bloom.h"
#include "dsp.h"
#include "engine.h"
#include "master/master.h"
#include "midiverb2.h"

#define MOD_BLOCK 256

/* ------------------------------------------------------------ TONE */
enum { TN_LONDON, TN_PUNCH, TN_CLEAN, TN_LACQUER, TN_CASSETTE, TN_SAMPLER, TN_TUBE, TN_FOLD, TN_N };
typedef struct {
    ms_engine ms;
    int chain;
    /* the character algorithms */
    Rng rng;
    Dly wow[2];
    float lp1[2], lp2[2], tilt[2], hold[2], hph, jit, wph, fph, env, drop, pink[2], dc[2], sub[2], subs[2], subz[2];
    Wander ww, dw;
    Svf sv[2];
    Bq bq[2][3];
    float bqp[3], bqf[3], bump;
} Tone;
int tone_init(Tone *t);
void tone_free(Tone *t);
void tone_reset(Tone *t, int algo);
void tone_process(Tone *t, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n);
int tone_latency(void);
float tone_gr_db(Tone *t);

/* ------------------------------------------------------------ MOD */
enum { MO_TRIPLE, MO_BARBER, MO_SPIRAL, MO_CHOIR, MO_DRIFT, MO_ORBIT, MO_LIQUID, MO_TIDE, MO_STRINGS, MO_N };
#define STR_BANDS 40
#define BARBER_VOICES 3
#define BARBER_STAGES 12
#define DRIFT_VOICES 6
typedef struct {
    Rng rng;
    /* triple resonator (PS-3100 style) */
    Svf rs[3];
    float rph, rsh[3], rsh_t[3], renv;
    Wander rw[3];
    /* barberpole phaser */
    float bph;                                     /* sweep phase 0..1 */
    float bz[2][BARBER_VOICES][BARBER_STAGES];     /* first-order all-pass states */
    float bfb[2][BARBER_VOICES];
    float bcoef[2][BARBER_VOICES];
    float bfl[2][BARBER_VOICES];
    Wander bw;
    /* spiral frequency shifter */
    float hx[2][2][4][2], hy[2][2][4][2];          /* [ch][path][stage][z1,z2] */
    float hd[2];                                   /* path 1's extra sample */
    float sph[2];                                  /* oscillator phase */
    Dly sdl[2];
    float slp[2], sfb_dc[2];
    float slfo;
    /* formant choir */
    Svf fs[2][3];
    float fph, cenv;
    Wander fw[2];
    /* drift ensemble */
    Dly edl[2];
    float eph[DRIFT_VOICES];
    Wander ew[DRIFT_VOICES];
    float elp[2], efb[2];
    /* orbit */
    Dly odl[2];
    float oph, olp[2];
    Wander ow;
    /* liquid (photocell vibe) */
    float vz[2][4], vlamp[2], vph, vfb[2], vwarm[2], vco[2][4];
    /* tide (dynamic harmonic tremolo) */
    float tph, tenv, tlo[2], tlo2[2];
    /* strings (Haible's 40-band string filter) */
    Svf sb[STR_BANDS];
    float sg[STR_BANDS], sgain[STR_BANDS], sfbk[2], svac, sscan;
    float gdt; /* SPIRAL's loop time, glided (tap_glide) */
    int gon;
} Modu;
int modu_init(Modu *m);
void modu_free(Modu *m);
void modu_reset(Modu *m, int algo);
void modu_process(Modu *m, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n);

/* ------------------------------------------------------------ DELAY */
enum { DL_GRAIN, DL_RHYTHM, DL_REVERSO, DL_TAPE, DL_SHIMMER, DL_CLOUD, DL_STUTTER, DL_DUB, DL_PRISM, DL_ERODE, DL_N };
#define GRAINS 32
typedef struct {
    int on;
    float pos, rate;            /* buffer position (absolute), step per sample (<0: reverse) */
    int len, age;
    float gl, gr;               /* pan gains */
} Grain;
typedef struct {
    Rng rng;
    Dly b[2];                   /* the delay memory: 8 s each side */
    float time_s;               /* glided delay time (samples) */
    float lp[2], hp[2];
    /* granular */
    Grain g[GRAINS];
    float gnext;                /* samples to the next grain */
    uint32_t wabs;              /* frames written (absolute position) */
    /* rhythm */
    Ap diff[2][2];
    Wander ww;
    float wph;
    /* reverso */
    float rc[2];                /* the reverse heads' counters */
    float env_f, env_s, swell;
    Ap rdiff[2][3];
    Wander rw;
    /* tape */
    float tph_w, tph_f;
    Wander tw;
    float tsat[2];
    /* shimmer */
    Dly psh[2];
    float shph;
    /* cloud */
    Dly cl[4];
    Ap cin[2][2];
    Wander cw[4];
    float clp[4], cduck;
    /* stutter */
    int st_on, st_left, st_rev, st_k;
    float st_pos, st_start, st_len, st_clock, st_gain, st_rate, st_pan;
    /* dub */
    Svf dsv[2];
    float dfb[2];
    /* prism: four bands, each its own time */
    Svf psv[2][4];
    Wander pw[4];
    float ptime[4];
    /* erode: the crumbling memory */
    float eh[2], eph, egate, etgt, ehiss[2];
    Wander ew;
    /* DUCK (deep): the input's level, the gain */
    float dk_env, dk_g;
    /* taps moved by SWING and SPREAD, glided (tap_glide); gon 0: snap to them first */
    float gsw, gsp;
    int gon;
} Delay;
int delay_init(Delay *d);
void delay_free(Delay *d);
void delay_reset(Delay *d, int algo);
void delay_process(Delay *d, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n);

/* ------------------------------------------------------------ REVERB */
enum { RV_CONCERT, RV_PLATE, RV_CHAMBER, RV_SPACE, RV_CLASSIC, RV_BLOOM, RV_MIDIVERB, RV_HALO, RV_SPRING, RV_INVERSE, RV_TEMPLE, RV_NEBULA, RV_N };
#define NEB_GRAINS 24
#define SPRING_AP 20
typedef struct {
    Rng rng;
    Dly pre[2];
    float bw_z[2][2];           /* COLOR: the band limit (two one-poles) */
    float obw_z[2][2];
    float size_g;               /* glided size */
    /* Dattorro / Lexicon-style tank (CONCERT, PLATE, HALO) */
    Ap din[4];
    Ap tap1[2], tap2[2];        /* the tank's modulated and plain all-passes */
    Dly td1[2], td2[2];
    float tdamp[2], tlow[2], tlast[2];
    float in_bw;
    Wander tw[4];
    float tph;
    /* shimmer */
    Dly sh;
    float shph, shfb[2];
    /* FDN 8x8 (CHAMBER) */
    Dly fd[8];
    float fdamp[8], flow[8];
    Ap fdin[2][2];
    Dly er[2];
    Wander fw[4];
    /* SPACE (multi-tap station) */
    Dly sp;
    Ap spin[3];
    float splp, spfb;
    Wander sw[2];
    /* CLASSIC (16 combs) */
    Dly cb[16];
    float cdamp[16];
    Ap cap[3];
    Dly cer[2];
    Bq clc[2];
    float clc_f;
    Wander cw;
    /* SPRING: two dispersive springs */
    Dly spd[2];
    float spap[2][SPRING_AP], sglp[2], sghp[2];
    Wander spw[2];
    /* INVERSE / gated (non-linear) */
    Dly inv;
    Ap invap[3];
    /* MidiVerb II Blooms */
    Bloom bloom;
    MV2 mv;
    Bq mvlo[2], mvhi[2];
    float mvlo_f, mvhi_f;
    /* TEMPLE: combs tuned to a chord, then diffused */
    Dly tc[2][4];
    float tcl[2][4];
    Ap tdf[2][3];
    Wander tcw[4];
    /* NEBULA: a granular cloud fed back into itself */
    Dly nb[2];
    Grain ng[NEB_GRAINS];
    float ngnext, nlp[2];
    Ap ndf[2][2];
    /* DUCK (deep): the input's level, the gain */
    float dk_env, dk_g;
    float gpre, gT; /* pre-delay and INVERSE's time, glided (tap_glide) */
    int gon, gon_t;
    float crc, cha, cnorm; /* CLASSIC's feedback, damping and level, glided (FREEZE snapped them) */
} Verb;
int verb_init(Verb *v);
void verb_free(Verb *v);
void verb_reset(Verb *v, int algo);
void verb_process(Verb *v, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n);
