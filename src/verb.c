/* SPDX-License-Identifier: GPL-3.0-or-later */
/* REVERB: twelve algorithms in the manner of late-80s / 90s studio units.
 *   CONCERT   a Lexicon-style figure-eight tank (after Dattorro's
 *             published plate topology) with "spin and wander" chorus
 *   PLATE     the same tank, smaller, denser and brighter
 *   CHAMBER   an 8x8 Hadamard feedback-delay network fed by an
 *             early-reflection pattern (a hybrid: reflections, then a tail;
 *             the idea after MAI_verb's FDN and HybridReverb2)
 *   SPACE     a recirculating multi-tap delay in the manner of the URSA
 *             Major Space Station: metallic, fluttering, rhythmic
 *   CLASSIC   sixteen parallel damped combs, a seven-tap early pattern and
 *             three diffusers: the structure and constants of AnClark's
 *             ClassicReverb RE-04 (GPL-3), a reverse-engineered Kjaerhus
 *             Classic Reverb
 *   BLOOM     the adjustable Alesis MidiVerb II "Bloom" (from Conduit)
 *   MIDIVERB  the MidiVerb II Bloom programs, emulated exactly (Conduit)
 *   HALO      the CONCERT tank with a pitch shifter in its feedback: each
 *             pass climbs an interval (shimmer)
 *   SPRING    two dispersive springs: chirping drips, wobble, tube drive
 *   INVERSE   the 80s/90s non-linear programs: a dense tap cloud that swells
 *             up (inverse) or is cut off (gated)
 *   TEMPLE    a room tuned to a chord: eight combs pitched to the chord's
 *             notes on a root, then diffused, so the tail rings in harmony
 *   NEBULA    a granular reverb: grains scattered from the last seconds,
 *             pitch-smeared (now and then an octave), fed back into the cloud
 * Every algorithm: pre-delay, and COLOR, the converters' era: from a 70s
 * unit's narrow band and few bits, through the 90s, to now. FREEZE (from
 * the patch) closes the input and lets the space ring forever. */
#include <stdio.h>

#include "modules.h"

#define K29 (48000.f / 29761.f) /* Dattorro's tank was tuned at 29761 Hz */
static const float DIN_LEN[4] = {142, 107, 379, 277};
static const float TL[2][4] = {{672, 4453, 1800, 3720}, {908, 4217, 2656, 3163}}; /* ap1 d1 ap2 d2 */

int verb_init(Verb *v)
{
    memset(v, 0, sizeof *v);
    v->rng.s = 0xb00b1e5u;
    int bad = 0;
    for (int c = 0; c < 2; c++) bad |= dly_init(&v->pre[c], TFX_SR * 6 / 10);
    for (int i = 0; i < 4; i++) bad |= ap_init(&v->din[i], (int)(DIN_LEN[i] * K29 * 2.5f), DIN_LEN[i] * K29);
    for (int c = 0; c < 2; c++) {
        bad |= ap_init(&v->tap1[c], (int)(TL[c][0] * K29 * 2.5f) + 128, TL[c][0] * K29);
        bad |= dly_init(&v->td1[c], (int)(TL[c][1] * K29 * 2.5f) + 128);
        bad |= ap_init(&v->tap2[c], (int)(TL[c][2] * K29 * 2.5f) + 128, TL[c][2] * K29);
        bad |= dly_init(&v->td2[c], (int)(TL[c][3] * K29 * 2.5f) + 128);
    }
    bad |= dly_init(&v->sh, TFX_SR / 4);
    for (int i = 0; i < 8; i++) bad |= dly_init(&v->fd[i], TFX_SR / 5 + 64);
    for (int c = 0; c < 2; c++) {
        bad |= ap_init(&v->fdin[c][0], 600, 211.f + c * 17);
        bad |= ap_init(&v->fdin[c][1], 900, 467.f + c * 29);
        bad |= dly_init(&v->er[c], TFX_SR * 3 / 10);
        bad |= dly_init(&v->cer[c], TFX_SR / 50);
        bad |= dly_init(&v->spd[c], TFX_SR / 5);
    }
    bad |= dly_init(&v->sp, TFX_SR * 13 / 10);
    static const float SPIN[3] = {142, 379, 587};
    for (int i = 0; i < 3; i++) bad |= ap_init(&v->spin[i], 1200, SPIN[i]);
    for (int i = 0; i < 16; i++) bad |= dly_init(&v->cb[i], (int)(0.004007f * 32.f * TFX_SR) + 64);
    static const float CAP[3] = {0.000200f, 0.000252f, 0.000317f};
    for (int i = 0; i < 3; i++) bad |= ap_init(&v->cap[i], 64, CAP[i] * TFX_SR);
    bad |= dly_init(&v->inv, TFX_SR + TFX_SR / 10);
    static const float INVAP[3] = {113, 337, 709};
    for (int i = 0; i < 3; i++) bad |= ap_init(&v->invap[i], 1000, INVAP[i]);
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < 4; k++) bad |= dly_init(&v->tc[c][k], TFX_SR / 8);
        static const float TDF[3] = {149, 263, 401};
        for (int k = 0; k < 3; k++) bad |= ap_init(&v->tdf[c][k], 1000, TDF[k] * (1.f + 0.13f * c));
        bad |= dly_init(&v->nb[c], TFX_SR * 3);
        bad |= ap_init(&v->ndf[c][0], 800, 241.f + 31.f * c);
        bad |= ap_init(&v->ndf[c][1], 1600, 677.f + 53.f * c);
    }
    bad |= bloom_init(&v->bloom, 1.024);
    mv2_init(&v->mv);
    v->size_g = -1.f;
    return bad ? -1 : 0;
}

void verb_free(Verb *v)
{
    for (int c = 0; c < 2; c++) {
        dly_free(&v->pre[c]); dly_free(&v->tap1[c].d); dly_free(&v->td1[c]); dly_free(&v->tap2[c].d); dly_free(&v->td2[c]);
        dly_free(&v->fdin[c][0].d); dly_free(&v->fdin[c][1].d); dly_free(&v->er[c]); dly_free(&v->cer[c]);
        dly_free(&v->spd[c]);
    }
    for (int i = 0; i < 4; i++) dly_free(&v->din[i].d);
    dly_free(&v->sh);
    for (int i = 0; i < 8; i++) dly_free(&v->fd[i]);
    dly_free(&v->sp);
    for (int i = 0; i < 3; i++) dly_free(&v->spin[i].d), dly_free(&v->cap[i].d), dly_free(&v->invap[i].d);
    for (int i = 0; i < 16; i++) dly_free(&v->cb[i]);
    dly_free(&v->inv);
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < 4; k++) dly_free(&v->tc[c][k]);
        for (int k = 0; k < 3; k++) dly_free(&v->tdf[c][k].d);
        dly_free(&v->nb[c]);
        dly_free(&v->ndf[c][0].d); dly_free(&v->ndf[c][1].d);
    }
    bloom_free(&v->bloom);
}

void verb_reset(Verb *v, int algo)
{
    (void)algo;
    for (int c = 0; c < 2; c++) {
        dly_clear(&v->pre[c]); dly_clear(&v->tap1[c].d); dly_clear(&v->td1[c]); dly_clear(&v->tap2[c].d);
        dly_clear(&v->td2[c]); dly_clear(&v->fdin[c][0].d); dly_clear(&v->fdin[c][1].d); dly_clear(&v->er[c]);
        dly_clear(&v->cer[c]); dly_clear(&v->spd[c]);
        v->tdamp[c] = v->tlow[c] = v->tlast[c] = v->shfb[c] = 0.f;
        v->sglp[c] = v->sghp[c] = 0.f;
        memset(v->spap[c], 0, sizeof v->spap[c]);
        v->clc[c].z1 = v->clc[c].z2 = v->mvlo[c].z1 = v->mvlo[c].z2 = v->mvhi[c].z1 = v->mvhi[c].z2 = 0.f;
        v->bw_z[c][0] = v->bw_z[c][1] = v->obw_z[c][0] = v->obw_z[c][1] = 0.f;
    }
    for (int i = 0; i < 4; i++) dly_clear(&v->din[i].d);
    dly_clear(&v->sh);
    for (int i = 0; i < 8; i++) dly_clear(&v->fd[i]), v->fdamp[i] = v->flow[i] = 0.f;
    dly_clear(&v->sp);
    v->splp = v->spfb = 0.f;
    for (int i = 0; i < 3; i++) dly_clear(&v->spin[i].d), dly_clear(&v->cap[i].d), dly_clear(&v->invap[i].d);
    for (int i = 0; i < 16; i++) dly_clear(&v->cb[i]), v->cdamp[i] = 0.f;
    dly_clear(&v->inv);
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < 4; k++) dly_clear(&v->tc[c][k]), v->tcl[c][k] = 0.f;
        for (int k = 0; k < 3; k++) dly_clear(&v->tdf[c][k].d);
        dly_clear(&v->nb[c]);
        dly_clear(&v->ndf[c][0].d); dly_clear(&v->ndf[c][1].d);
        v->nlp[c] = 0.f;
    }
    memset(v->ng, 0, sizeof v->ng);
    v->ngnext = 0.f;
    bloom_reset(&v->bloom);
    mv2_reset(&v->mv);
    v->size_g = -1.f;
}

static float rt60(float p, float lo, float hi) { return expmap(p, lo, hi); }
/* the gain a loop of `secs` keeps per trip for a reverb time of rt seconds */
static float loop_gain(float secs, float rt) { return powf(10.f, -3.f * secs / rt); }

/* ---------------------------------------------------------------- the tank: CONCERT, PLATE, HALO */
static const float SHI[5] = {12, 7, 19, 24, -12};
static const char *const SHIN[5] = {"+OCTAVE", "+FIFTH", "+OCT+5TH", "+2 OCT", "-OCTAVE"};

static void tank(Verb *v, int algo, const float *p, const float *raw, int freeze, float *l, float *r, int n)
{
    float smin = 0.5f, smax = 2.f, rlo = 0.4f, rhi = 30.f;
    if (algo == RV_PLATE) smin = 0.3f, smax = 1.f, rlo = 0.3f, rhi = 12.f;
    if (algo == RV_HALO) smin = 1.f, smax = 2.3f, rlo = 1.f, rhi = 40.f;
    float size = smin + (smax - smin) * p[1];
    if (v->size_g < 0.f) v->size_g = size;
    float RT = rt60(p[0], rlo, rhi);
    float cut = expmap(1.f - p[2], 1200.f, 18000.f), dk = freeze ? 1.f : onepole_coef(cut);
    float mod = p[3] * 28.f;
    /* DEEP: X-OVER (where BASS takes over), SPIN RATE; CONCERT / PLATE: WANDER (random against
     * regular), TANK DIFF (the tank's second all-passes); HALO: DIFFUSION, WINDOW (the shifter's) */
    int halo = algo == RV_HALO;
    float bass = halo ? 1.2f : expmap(p[6], 0.5f, 2.5f);
    float diff = halo ? p[10] : p[7];
    float wand = halo ? 0.4f : p[10], spin = expmap(p[9], 0.1f, 4.f);
    float gin1 = lerpf(0.45f, algo == RV_PLATE ? 0.82f : 0.75f, diff), gin2 = gin1 * 0.83f;
    float gd1 = lerpf(0.45f, algo == RV_PLATE ? 0.78f : 0.7f, diff), gd2 = halo ? 0.5f : 0.2f + 0.5f * p[11];
    float shimmer = algo == RV_HALO ? p[6] * 0.75f : 0.f;
    float ratio = algo == RV_HALO ? exp2f(SHI[pstep(raw[7], 5)] / 12.f) : 1.f;
    float lk = onepole_coef(expmap(p[8], 80.f, 1000.f));
    for (int i = 0; i < n; i++) {
        v->size_g += (size - v->size_g) * 0.0004f;
        float s = v->size_g * K29;
        float half = (TL[0][0] + TL[0][1] + TL[0][2] + TL[0][3]) * s / TFX_SR;
        /* decay per multiplier (two per half loop), for highs and lows */
        /* HALO: the shifter puts energy back in, so the tank keeps a little less itself */
        float dec = freeze ? 1.f : sqrtf(loop_gain(half, RT)) * (1.f - 0.12f * shimmer);
        float decl = freeze ? 1.f : fminf(sqrtf(loop_gain(half, RT * bass)), 0.9995f);
        v->tph += spin / TFX_SR;
        v->tph -= floorf(v->tph);
        float m0 = mod * ((1.f - wand) * lfo_sin(v->tph) + wand * wander_tick(&v->tw[0], &v->rng, 0.7f));
        float m1 = mod * ((1.f - wand) * lfo_sin(v->tph + 0.25f) + wand * wander_tick(&v->tw[1], &v->rng, 0.55f));
        float m2 = mod * 0.3f * wander_tick(&v->tw[2], &v->rng, 0.37f);
        float m3 = mod * 0.3f * wander_tick(&v->tw[3], &v->rng, 0.43f);
        float x = freeze ? 0.f : 0.5f * (l[i] + r[i]);
        /* shimmer: two crossfading heads over a 100 ms window */
        if (shimmer > 0.f) {
            float W = expmap(p[11], 0.03f, 0.2f) * TFX_SR;
            dly_push(&v->sh, 0.5f * (v->shfb[0] + v->shfb[1]));
            v->shph += (1.f - ratio) / W;
            v->shph -= floorf(v->shph);
            float ps = 0.f;
            for (int h = 0; h < 2; h++) {
                float ph = v->shph + 0.5f * h;
                ph -= floorf(ph);
                ps += (0.5f - 0.5f * lfo_sin(ph + 0.25f)) * dly_lin(&v->sh, 2.f + ph * W);
            }
            x += shimmer * softclip(ps);
        }
        for (int k = 0; k < 4; k++) {
            v->din[k].len = DIN_LEN[k] * K29 * (0.6f + 0.4f * v->size_g);
            x = ap_run(&v->din[k], x, k < 2 ? gin1 : gin2);
        }
        float last[2] = {v->tlast[1], v->tlast[0]}; /* each half is fed by the other */
        for (int c = 0; c < 2; c++) {
            float y = x + last[c];
            y = ap_run_mod(&v->tap1[c], y, -gd1, TL[c][0] * s + (c ? m1 : m0) + 30.f);
            dly_push(&v->td1[c], y);
            y = dly_lin(&v->td1[c], TL[c][1] * s + (c ? m3 : m2) + 4.f);
            v->tdamp[c] += dk * (y - v->tdamp[c]);
            v->tlow[c] += lk * (v->tdamp[c] - v->tlow[c]);
            y = (v->tdamp[c] - v->tlow[c]) * dec + v->tlow[c] * decl;
            v->tap2[c].len = TL[c][2] * s;
            y = ap_run(&v->tap2[c], y, gd2);
            dly_push(&v->td2[c], y);
            v->tlast[c] = dly_lin(&v->td2[c], TL[c][3] * s) * dec;
        }
        /* Dattorro's output taps, scaled with the room */
        Dly *L1 = &v->td1[0], *L2 = &v->td2[0], *R1 = &v->td1[1], *R2 = &v->td2[1];
        Dly *LA = &v->tap2[0].d, *RA = &v->tap2[1].d;
#define T(d, k) dly_lin(d, (k) * s + 1.f)
        float yl = T(R1, 266) + T(R1, 2974) - T(RA, 1913) + T(R2, 1996) - T(L1, 1990) - T(LA, 187) - T(L2, 1066);
        float yr = T(L1, 353) + T(L1, 3627) - T(LA, 1228) + T(L2, 2673) - T(R1, 2111) - T(RA, 335) - T(R2, 121);
#undef T
        v->shfb[0] = yl; v->shfb[1] = yr;
        l[i] = yl * 0.42f;
        r[i] = yr * 0.42f;
    }
}

/* ---------------------------------------------------------------- CHAMBER: early pattern + FDN 8x8 */
static const float FDL[8] = {29.7f, 37.1f, 41.1f, 43.7f, 53.9f, 59.3f, 67.9f, 73.1f}; /* ms */
static const char *const ERN[4] = {"ROOM", "HALL", "CATHEDRAL", "SLAPBACK"};

static void er_tap(int shape, int k, float *ms, float *g, float *pan)
{
    static const float JIT[12] = {0.13f, -0.31f, 0.27f, -0.08f, 0.41f, -0.22f, 0.05f, 0.36f, -0.4f, 0.18f, -0.12f, 0.29f};
    static const float SP[4] = {3.1f, 7.5f, 13.f, 2.f}, CU[4] = {0.15f, 0.35f, 0.9f, 0.f}, DE[4] = {0.86f, 0.9f, 0.93f, 0.75f};
    static const float ST[4] = {4.f, 9.f, 18.f, 60.f};
    *ms = ST[shape] + k * SP[shape] * (1.f + JIT[k] * 0.5f) + k * k * CU[shape];
    if (shape == 3) *ms = k < 2 ? 60.f + k * 37.f : 140.f + k * 5.f * (1.f + JIT[k]); /* two loud slaps, then a smear */
    *g = powf(DE[shape], (float)k) * (shape == 3 && k < 2 ? 1.6f : 1.f);
    *pan = (k & 1 ? 1.f : -1.f) * (0.35f + 0.6f * fabsf(JIT[k]) * 1.5f);
}

static void chamber(Verb *v, const float *p, const float *raw, int freeze, float *l, float *r, int n)
{
    float size = expmap(p[1], 0.4f, 2.5f);
    if (v->size_g < 0.f) v->size_g = size;
    float RT = rt60(p[0], 0.3f, 25.f);
    /* DEEP: X-OVER, LOW DECAY (the lows' time against DECAY), DIFFUSION (before the network),
     * WANDER RATE */
    float dk = freeze ? 1.f : onepole_coef(expmap(1.f - p[2], 1500.f, 18000.f)), lk = onepole_coef(expmap(p[8], 80.f, 1000.f));
    float lowm = expmap(p[9], 0.5f, 2.5f), dif = fminf(p[10] * 1.2f, 0.95f), wsc = expmap(p[11], 0.25f, 4.f);
    float mod = p[3] * 14.f, er = p[6] * 1.4f;
    int shape = pstep(raw[7], 4);
    float ems[12], eg[12], ep[12];
    for (int k = 0; k < 12; k++) er_tap(shape, k, &ems[k], &eg[k], &ep[k]);
    float gn[8], gl[8];
    for (int i = 0; i < 8; i++) {
        float secs = FDL[i] * 0.001f * size;
        gn[i] = freeze ? 1.f : loop_gain(secs, RT);
        gl[i] = freeze ? 1.f : fminf(loop_gain(secs, RT * lowm), 0.9995f);
    }
    for (int i = 0; i < n; i++) {
        v->size_g += (size - v->size_g) * 0.0004f;
        float xs[2] = {freeze ? 0.f : l[i], freeze ? 0.f : r[i]}, erl = 0.f, err = 0.f;
        dly_push(&v->er[0], xs[0]);
        dly_push(&v->er[1], xs[1]);
        for (int k = 0; k < 12; k++) {
            float d = ems[k] * 0.001f * TFX_SR * (0.5f + 0.5f * v->size_g);
            float a = dly_lin(&v->er[0], d), b = dly_lin(&v->er[1], d);
            float m = 0.5f * (a + b);
            erl += eg[k] * m * sqrtf(0.5f * (1.f - ep[k]));
            err += eg[k] * m * sqrtf(0.5f * (1.f + ep[k]));
        }
        erl *= 0.3f; err *= 0.3f;
        float in[2];
        for (int c = 0; c < 2; c++) {
            float x = xs[c] + (c ? err : erl) * 0.6f;
            x = ap_run(&v->fdin[c][0], x, dif);
            in[c] = ap_run(&v->fdin[c][1], x, dif * 0.9167f);
        }
        float y[8];
        for (int k = 0; k < 8; k++) {
            float md = k < 4 ? mod * wander_tick(&v->fw[k], &v->rng, (0.3f + 0.17f * k) * wsc) : 0.f;
            float z = dly_lin(&v->fd[k], FDL[k] * 0.001f * TFX_SR * v->size_g + md + 16.f);
            v->fdamp[k] += dk * (z - v->fdamp[k]);
            v->flow[k] += lk * (v->fdamp[k] - v->flow[k]);
            y[k] = (v->fdamp[k] - v->flow[k]) * gn[k] + v->flow[k] * gl[k];
        }
        /* a fast Walsh-Hadamard transform: every line feeds every other, energy kept */
        float h[8];
        memcpy(h, y, sizeof h);
        for (int len = 1; len < 8; len <<= 1)
            for (int a = 0; a < 8; a += len << 1)
                for (int b = a; b < a + len; b++) {
                    float u = h[b], w = h[b + len];
                    h[b] = u + w; h[b + len] = u - w;
                }
        for (int k = 0; k < 8; k++)
            dly_push(&v->fd[k], h[k] * 0.35355339f + (k & 1 ? in[1] : in[0]) * (k & 2 ? -0.5f : 0.5f));
        float ol = y[0] - y[2] + y[4] - y[6] + 0.5f * (y[1] - y[5]);
        float orr = y[1] - y[3] + y[5] - y[7] + 0.5f * (y[2] - y[6]);
        l[i] = ol * 0.3f + erl * er;
        r[i] = orr * 0.3f + err * er;
    }
}

/* ---------------------------------------------------------------- SPACE: the station's recirculating taps */
static const float SPT[4][8] = {
    {23, 47, 71, 97, 131, 163, 199, 241},
    {30, 60, 90, 120, 150, 180, 210, 240},
    {13, 21, 34, 55, 89, 144, 233, 377},
    {61, 67, 71, 79, 149, 157, 163, 173},
};
static const char *const SPN[4] = {"SPACE", "ECHO-VERB", "GOLDEN", "CLUSTER"};

static void space(Verb *v, const float *p, const float *raw, int freeze, float *l, float *r, int n)
{
    /* DEEP: WANDER RATE, DRIVE (the loop's saturation) */
    float wsc = expmap(p[8], 0.25f, 4.f), sdrive = expmap(p[9], 1.f, 6.f);
    int mode = pstep(raw[6], 4);
    float size = expmap(p[1], 0.4f, 2.f);
    if (v->size_g < 0.f) v->size_g = size;
    float RT = rt60(p[0], 0.3f, 20.f);
    float dk = freeze ? 1.f : onepole_coef(expmap(1.f - p[2], 1500.f, 16000.f));
    float mod = p[3] * 40.f, dens = p[7] * 0.7f;
    float mean = 0.f;
    for (int k = 0; k < 8; k++) mean += SPT[mode][k];
    mean = mean / 8.f * 0.001f * size;
    /* each tap gives an eighth; the loop's gain overall sets the time */
    float g = freeze ? 1.f : fminf(loop_gain(mean, RT) * 1.6f, 0.995f);
    static const float SG[8] = {1, -1, 1, 1, -1, 1, -1, 1};
    for (int i = 0; i < n; i++) {
        v->size_g += (size - v->size_g) * 0.0003f;
        float x = freeze ? 0.f : 0.5f * (l[i] + r[i]);
        for (int k = 0; k < 3; k++) x = ap_run(&v->spin[k], x, dens);
        float acc = 0.f, ol = 0.f, orr = 0.f;
        float w0 = wander_tick(&v->sw[0], &v->rng, 0.6f * wsc) * mod, w1 = wander_tick(&v->sw[1], &v->rng, 1.1f * wsc) * mod;
        for (int k = 0; k < 8; k++) {
            float t = dly_lin(&v->sp, SPT[mode][k] * 0.001f * TFX_SR * v->size_g + (k & 1 ? w0 : w1) * (0.3f + 0.1f * k) + 2.f);
            acc += SG[k] * t;
            if (k & 1) orr += t * (0.6f + 0.05f * k); else ol += t * (0.6f + 0.05f * k);
        }
        v->splp += dk * (acc * 0.125f - v->splp);
        v->spfb = softclip(v->splp * g * sdrive) / sdrive;
        dly_push(&v->sp, x + v->spfb * (freeze ? 1.f : 1.f) * 1.f);
        l[i] = ol * 0.6f;
        r[i] = orr * 0.6f;
    }
}

/* ---------------------------------------------------------------- CLASSIC (RE-04's structure) */
static const float CER_D[7] = {0.000597f, 0.001340f, 0.001610f, 0.002160f, 0.002450f, 0.002820f, 0.003520f};
static const float CER_G[7] = {0.65f, 0.45f, 0.41f, 0.34f, 0.31f, 0.28f, 0.24f};
static const float CB_D[16] = {0.002092f, 0.002185f, 0.002281f, 0.002382f, 0.002488f, 0.002598f, 0.002713f, 0.002833f,
                               0.002959f, 0.003090f, 0.003226f, 0.003369f, 0.003518f, 0.003674f, 0.003837f, 0.004007f};
static const float CB_L[16] = {+0.19850f, -0.12350f, +0.30150f, -0.13230f, +0.16650f, +0.34160f, +0.46740f, -0.06785f,
                               -0.18790f, +0.37030f, +0.02341f, -0.39260f, +0.15080f, +0.22060f, -0.08366f, -0.25290f};
static const float CB_R[16] = {+0.17920f, +0.34930f, +0.29600f, -0.02510f, -0.08528f, -0.23830f, +0.35500f, +0.37310f,
                               +0.04845f, -0.02328f, -0.35360f, +0.29490f, -0.03428f, +0.32320f, -0.26350f, -0.19590f};

static void classic(Verb *v, const float *p, int freeze, float *l, float *r, int n)
{
    /* RE-04: room size 0..1 scales every comb by 32^room; "damping" sets the combs'
     * feedback on a log-log curve; hi-damp sweeps a one-pole from 20 kHz down 2 decades */
    /* DEEP: WANDER RATE, DIFFUSER (the three all-passes' gain), STEREO (RE-04's L / R tap gains
     * against a mono sum) */
    float wrate = expmap(p[8], 0.1f, 2.5f), capg = p[9] * 0.8f, ster = p[10];
    float room = p[1];
    if (v->size_g < 0.f) v->size_g = room;
    const float gmax = 0.98f, gmin = 0.40f, alpha = -log10f(gmax), ratio = -log10f(gmin) / alpha;
    float rc = freeze ? 1.f : powf(10.f, -alpha * powf(ratio, 1.f - p[0]));
    float fc = 20000.f * expf(-p[2] * logf(100.f));
    float ha = freeze ? 1.f : 1.f - expf(-TFX_2PI * fc / TFX_SR);
    float mod = p[3] * 6.f, erg = p[6] * 1.5f;
    float lc = expmap(p[7], 20.f, 1000.f);
    if (lc != v->clc_f) { v->clc_f = lc; bq_set(&v->clc[0], BQ_HP, lc, 0.7071f, 0); bq_set(&v->clc[1], BQ_HP, lc, 0.7071f, 0); }
    float norm = 0.55f * sqrtf(1.f - fminf(rc, 0.999f)) + 0.02f;
    for (int i = 0; i < n; i++) {
        v->size_g += (room - v->size_g) * 0.0003f;
        float scale = powf(32.f, v->size_g);
        float xl = freeze ? 0.f : l[i], xr = freeze ? 0.f : r[i];
        dly_push(&v->cer[0], xl);
        dly_push(&v->cer[1], xr);
        float erl = 0.f, err = 0.f;
        for (int t = 0; t < 7; t++) {
            erl += dly_tap(&v->cer[0], (uint32_t)(CER_D[t] * TFX_SR)) * CER_G[t];
            err += dly_tap(&v->cer[1], (uint32_t)(CER_D[t] * TFX_SR)) * CER_G[t];
        }
        float a = xl + xr + 6e-8f * rng_bi(&v->rng);
        for (int k = 0; k < 3; k++) a = ap_run(&v->cap[k], a, capg);
        float w = mod > 0.f ? mod * wander_tick(&v->cw, &v->rng, wrate) : 0.f;
        float ol = 0.f, orr = 0.f;
        for (int c = 0; c < 16; c++) {
            float len = CB_D[c] * scale * TFX_SR + ((c & 3) == 0 ? w : 0.f);
            float z = dly_lin(&v->cb[c], len < 2.f ? 2.f : len);
            v->cdamp[c] = v->cdamp[c] * (1.f - ha) + z * ha;
            dly_push(&v->cb[c], a + v->cdamp[c] * rc);
            float mono = 0.5f * (CB_L[c] + CB_R[c]);
            ol += z * lerpf(mono, CB_L[c], ster);
            orr += z * lerpf(mono, CB_R[c], ster);
        }
        l[i] = bq_run(&v->clc[0], ol * norm) + erl * erg * 0.5f;
        r[i] = bq_run(&v->clc[1], orr * norm) + err * erg * 0.5f;
    }
}

/* ---------------------------------------------------------------- BLOOM and MIDIVERB (Conduit's) */
static float bloom_decay(float seconds, float size)
{
    float T = clampf(seconds, 1.f, 30.f);
    return expf(-4.2f * size / T) / 0.75f;
}

static void blooms(Verb *v, int algo, const float *p, const float *raw, int freeze, float *l, float *r, int n)
{
    float buf[2 * MOD_BLOCK];
    for (int i = 0; i < n; i++) buf[2 * i] = freeze ? 0.f : l[i], buf[2 * i + 1] = freeze ? 0.f : r[i];
    if (algo == RV_BLOOM) {
        float dm = p[2], pole = dm <= 0.5f ? dm : 0.5f + (dm - 0.5f) * 0.8f;
        bloom_set(&v->bloom, 0.5f + p[1], 0.25f + 0.5f * p[6], freeze ? 0.f : pole);
        float dec = freeze ? 1.f / 0.75f : bloom_decay(expmap(p[0], 1.f, 30.f), v->bloom.size);
        bloom_process(&v->bloom, pstep(raw[7], 2), dec, buf, buf, n);
        for (int i = 0; i < n; i++) l[i] = buf[2 * i] * 1.4f, r[i] = buf[2 * i + 1] * 1.4f;
    } else {
        mv2_process(&v->mv, pstep(raw[0], 2) ? 49 : 45, buf, buf, n);
        float lo = expmap(p[1], 20.f, 1000.f), hi = expmap(p[2], 1000.f, 16000.f), wd = p[3] * 1.5f;
        if (lo != v->mvlo_f) { v->mvlo_f = lo; for (int c = 0; c < 2; c++) bq_set(&v->mvlo[c], BQ_HP, lo, 0.7071f, 0); }
        if (hi != v->mvhi_f) { v->mvhi_f = hi; for (int c = 0; c < 2; c++) bq_set(&v->mvhi[c], BQ_LP, hi, 0.7071f, 0); }
        for (int i = 0; i < n; i++) {
            float a = bq_run(&v->mvhi[0], bq_run(&v->mvlo[0], buf[2 * i]));
            float b = bq_run(&v->mvhi[1], bq_run(&v->mvlo[1], buf[2 * i + 1]));
            float m = 0.5f * (a + b), s = 0.5f * (a - b) * wd;
            l[i] = (m + s) * 1.6f;
            r[i] = (m - s) * 1.6f;
        }
    }
}

/* ---------------------------------------------------------------- SPRING */
static void spring(Verb *v, const float *p, int freeze, float *l, float *r, int n)
{
    /* DEEP: LOW CUT, PAIR (the second spring's length against the first), WOBBLE RATE, CHIRP (how
     * many dispersion stages: the drip's length) */
    float ratio = 1.f + p[9] * 0.5f, wrate = expmap(p[10], 0.3f, 8.f);
    int stages = 4 + (int)(p[11] * (SPRING_AP - 4) + 0.5f);
    float RT = rt60(p[0], 0.5f, 8.f);
    float len = expmap(p[1], 0.018f, 0.07f) * TFX_SR;
    float dk = onepole_coef(expmap(1.f - p[2], 1800.f, 6500.f)), hk = onepole_coef(expmap(p[8], 30.f, 400.f));
    float wob = p[3] * 9.f, drip = 0.45f + 0.28f * p[6], drive = 1.f + p[7] * 6.f;
    for (int i = 0; i < n; i++) {
        float x = freeze ? 0.f : 0.5f * (l[i] + r[i]);
        x = softclip(x * drive) / (0.6f + 0.4f * drive); /* the tube driver */
        for (int c = 0; c < 2; c++) {
            float lc = len * (c ? ratio : 1.f);
            float g = freeze ? 1.f : loop_gain(lc / TFX_SR, RT);
            float d = dly_lin(&v->spd[c], lc + wob * wander_tick(&v->spw[c], &v->rng, wrate) + 4.f);
            /* the dispersion: a chain of first-order all-passes, lows arrive late (the chirp) */
            float y = d;
            for (int k = 0; k < stages; k++) {
                float o = drip * y + v->spap[c][k];
                v->spap[c][k] = y - drip * o;
                y = o;
            }
            v->sglp[c] += dk * (y - v->sglp[c]);
            v->sghp[c] += hk * (v->sglp[c] - v->sghp[c]);
            y = v->sglp[c] - v->sghp[c];
            dly_push(&v->spd[c], x + softclip(y * g));
            float o = y * 0.9f;
            if (c) r[i] = o; else l[i] = o;
        }
    }
}

/* ---------------------------------------------------------------- INVERSE / GATED */
#define INV_TAPS 32
static void inverse(Verb *v, const float *p, int freeze, float *l, float *r, int n)
{
    float T = expmap(p[0], 0.08f, 0.9f) * TFX_SR, shape = p[1];
    float dk = onepole_coef(expmap(1.f - p[2], 2000.f, 18000.f));
    float dens = 0.3f + 0.45f * p[6], spread = p[7];
    /* DEEP: TAPS (8-32: few is grainy, many smooth), JITTER (the taps' irregularity) */
    int ntap = 8 + (int)(p[8] * (INV_TAPS - 8) + 0.5f);
    float jit = p[9];
    static const float JIT[INV_TAPS] = {0.1f, 0.7f, 0.3f, 0.9f, 0.2f, 0.5f, 0.8f, 0.4f, 0.6f, 0.0f, 0.75f, 0.35f, 0.15f, 0.95f, 0.55f, 0.25f,
                                        0.65f, 0.05f, 0.85f, 0.45f, 0.3f, 0.7f, 0.1f, 0.9f, 0.5f, 0.2f, 0.8f, 0.4f, 0.6f, 0.0f, 0.75f, 0.35f};
    float gk[INV_TAPS], dt[INV_TAPS], tot = 0.f;
    for (int k = 0; k < INV_TAPS; k++) {
        if (k >= ntap) { gk[k] = 0.f; dt[k] = 2.f; continue; }
        float t = ((float)k + 0.5f + (JIT[k] - 0.5f) * jit) / (float)ntap;
        dt[k] = 2.f + t * T;
        float gated = 1.f - 0.25f * t, inv = t * t;
        gk[k] = lerpf(gated, inv, shape) * (k & 1 ? -1.f : 1.f);
        tot += fabsf(gk[k]);
    }
    float norm = 2.2f / tot;
    for (int i = 0; i < n; i++) {
        float x = freeze ? 0.f : 0.5f * (l[i] + r[i]);
        for (int k = 0; k < 3; k++) x = ap_run(&v->invap[k], x, dens);
        if (!freeze) dly_push(&v->inv, x);
        float ol = 0.f, orr = 0.f;
        for (int k = 0; k < ntap; k++) {
            float y = dly_lin(&v->inv, dt[k]) * gk[k];
            float pan = (k & 2 ? 1.f : -1.f) * spread;
            ol += y * (1.f - pan * 0.5f);
            orr += y * (1.f + pan * 0.5f);
        }
        v->tdamp[0] += dk * (ol * norm - v->tdamp[0]);
        v->tdamp[1] += dk * (orr * norm - v->tdamp[1]);
        l[i] = v->tdamp[0];
        r[i] = v->tdamp[1];
    }
}

/* ---------------------------------------------------------------- TEMPLE */
static const float CHORD[8][4] = {
    {0, 12, 24, -12}, {0, 7, 12, 19}, {0, 3, 7, 10}, {0, 4, 7, 11},
    {0, 5, 7, 14}, {0, 2, 4, 6}, {0, 1, 5, 7}, {0, 1, 2, 3},
};
static const char *const CHORDN[8] = {"OCTAVES", "FIFTHS", "MINOR 7", "MAJOR 7", "SUS", "WHOLE TONE", "PELOG", "CLUSTER"};
static const char *const NOTEN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static float temple_root(float v) { return 32.7032f * exp2f(floorf(v * 36.f + 0.5f) / 12.f); } /* C1 .. C4, in semitones */

static void temple(Verb *v, const float *p, const float *raw, int freeze, float *l, float *r, int n)
{
    /* DECAY, ROOT, DAMP, DRIFT (the combs' pitch wanders: a choir of rooms), PRE-DELAY, COLOR,
     * DIFFUSION (from bare ringing combs to a room), CHORD
     * DEEP: SPREAD (left and right a little apart in pitch), DUCK, WIDTH */
    float RT = rt60(p[0], 0.5f, 30.f), root = temple_root(p[1]);
    float dk = freeze ? 1.f : onepole_coef(expmap(1.f - p[2], 700.f, 16000.f));
    float drift = p[3] * 0.012f, diff = p[6] * 0.72f, spread = p[8] * 0.012f;
    const float *ch = CHORD[pstep(raw[7], 8)];
    float per[2][4], g[2][4], nrm[2][4];
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 4; k++) {
            float f = root * exp2f(ch[k] / 12.f) * (1.f + (c ? spread : -spread));
            per[c][k] = clampf(TFX_SR / f, 8.f, TFX_SR / 8.f - 8.f);
            g[c][k] = freeze ? 1.f : loop_gain(per[c][k] / TFX_SR, RT);
            nrm[c][k] = sqrtf(1.f - g[c][k] * g[c][k] * 0.999f) * 1.6f;
        }
    for (int i = 0; i < n; i++) {
        float w[4];
        for (int k = 0; k < 4; k++) w[k] = 1.f + drift * wander_tick(&v->tcw[k], &v->rng, 0.3f + 0.1f * k);
        float x[2] = {freeze ? 0.f : l[i], freeze ? 0.f : r[i]};
        for (int c = 0; c < 2; c++) {
            float acc = 0.f;
            for (int k = 0; k < 4; k++) {
                float y = dly_lin(&v->tc[c][k], per[c][k] * w[k]);
                v->tcl[c][k] += dk * (y - v->tcl[c][k]);
                dly_push(&v->tc[c][k], x[c] * nrm[c][k] + v->tcl[c][k] * g[c][k]);
                acc += y;
            }
            acc *= 0.5f;
            for (int k = 0; k < 3; k++) acc = lerpf(acc, ap_run(&v->tdf[c][k], acc, diff), diff > 0.01f ? 1.f : 0.f);
            if (c) r[i] = acc; else l[i] = acc;
        }
    }
}

/* ---------------------------------------------------------------- NEBULA */
static void nebula(Verb *v, const float *p, int freeze, float *l, float *r, int n)
{
    /* DECAY (how much of the cloud goes round again), SIZE (the grains' length and how far back they
     * reach), DAMP, SCATTER (pitch and place: from a smooth wash to a shimmering swarm), PRE-DELAY,
     * COLOR, DENSITY, OCTAVES (how often a grain jumps an octave up or down)
     * DEEP: REVERSE (how often a grain plays backwards), DIFFUSION (before the cloud), DUCK, WIDTH */
    float fb = freeze ? 0.98f : p[0] * 0.92f;
    float len = expmap(p[1], 0.04f, 0.5f) * TFX_SR, reach = len * 4.f;
    float dk = freeze ? 1.f : onepole_coef(expmap(1.f - p[2], 1500.f, 18000.f));
    float scat = p[3], dens = expmap(p[6], 4.f, 80.f), octv = p[7] * 0.5f, rev = p[8], dif = p[9] * 0.75f;
    float over = dens * len / TFX_SR, norm = 1.6f / sqrtf(over > 1.f ? over : 1.f);
    const float N = (float)(v->nb[0].mask + 1);
    if (reach > N - len * 2.f - TFX_SR / 10.f) reach = N - len * 2.f - TFX_SR / 10.f;
    for (int i = 0; i < n; i++) {
        v->ngnext -= 1.f;
        if (v->ngnext <= 0.f) {
            v->ngnext += TFX_SR / dens * (0.5f + rng_uni(&v->rng));
            for (int k = 0; k < NEB_GRAINS; k++) {
                Grain *g = &v->ng[k];
                if (g->on) continue;
                float cents = scat * 40.f * rng_bi(&v->rng);
                float ratio = exp2f(cents / 1200.f);
                if (rng_uni(&v->rng) < octv) ratio *= rng_uni(&v->rng) < 0.6f ? 2.f : 0.5f;
                g->len = (int)(len * (0.6f + 0.8f * rng_uni(&v->rng)));
                int back = rng_uni(&v->rng) < rev;
                float from = len + reach * (0.15f + 0.85f * scat) * rng_uni(&v->rng) + 64.f;
                if (!back && ratio > 1.f) from += (ratio - 1.f) * g->len;
                float pos = (float)v->nb[0].w - from;
                while (pos < 0.f) pos += N;
                g->pos = pos;
                g->rate = back ? -ratio : ratio;
                g->age = 0;
                float pan = rng_bi(&v->rng) * (0.3f + 0.7f * scat);
                g->gl = sqrtf(0.5f * (1.f - pan)) * norm;
                g->gr = sqrtf(0.5f * (1.f + pan)) * norm;
                g->on = 1;
                break;
            }
        }
        float ol = 0.f, orr = 0.f;
        for (int k = 0; k < NEB_GRAINS; k++) {
            Grain *g = &v->ng[k];
            if (!g->on) continue;
            float w = 0.5f - 0.5f * lfo_sin((float)g->age / (float)g->len + 0.25f);
            ol += w * g->gl * dly_at(&v->nb[0], g->pos);
            orr += w * g->gr * dly_at(&v->nb[1], g->pos);
            g->pos += g->rate;
            if (g->pos >= N) g->pos -= N;
            if (g->pos < 0.f) g->pos += N;
            if (++g->age >= g->len) g->on = 0;
        }
        v->nlp[0] += dk * (ol - v->nlp[0]);
        v->nlp[1] += dk * (orr - v->nlp[1]);
        float x[2] = {freeze ? 0.f : l[i], freeze ? 0.f : r[i]};
        for (int c = 0; c < 2; c++) {
            float in = x[c];
            if (dif > 0.01f) in = ap_run(&v->ndf[c][1], ap_run(&v->ndf[c][0], in, dif), dif * 0.9f);
            dly_push(&v->nb[c], in + fb * softclip(v->nlp[c]));
        }
        l[i] = v->nlp[0];
        r[i] = v->nlp[1];
    }
}

/* ---------------------------------------------------------------- the frame: pre-delay, COLOR */
void verb_process(Verb *v, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    int freeze = pt->freeze_verb;
    /* DEEP, every algorithm: DUCK (14) and WIDTH (15) */
    float duck = p[14], width = p[15] * 2.f, lev[MOD_BLOCK];
    if (duck > 0.001f)
        for (int i = 0; i < n; i++) lev[i] = 0.5f * (fabsf(l[i]) + fabsf(r[i]));
    /* the per-algorithm parameter slots for pre-delay and COLOR (MIDIVERB has neither) */
    float pre = algo == RV_MIDIVERB ? 0.f : p[4] * 0.5f * TFX_SR;
    float era = algo == RV_MIDIVERB ? 1.f : p[5];
    float bw = expmap(era, 5000.f, 20000.f), bk = onepole_coef(bw);
    int crush = era < 0.5f;
    float q = crush ? exp2f(-(7.f + 18.f * era)) : 0.f; /* 7 bits at the bottom .. 16 at the 90s */
    for (int i = 0; i < n; i++) {
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float y = x[c];
            if (pre >= 1.f) { dly_push(&v->pre[c], y); y = dly_lin(&v->pre[c], pre); }
            if (era < 0.98f) {
                v->bw_z[c][0] += bk * (y - v->bw_z[c][0]);
                v->bw_z[c][1] += bk * (v->bw_z[c][0] - v->bw_z[c][1]);
                y = v->bw_z[c][1];
            }
            if (crush) y = q * floorf(y / q + 0.5f);
            if (c) r[i] = y; else l[i] = y;
        }
    }
    switch (algo) {
    case RV_CONCERT: case RV_PLATE: case RV_HALO: tank(v, algo, p, raw, freeze, l, r, n); break;
    case RV_CHAMBER: chamber(v, p, raw, freeze, l, r, n); break;
    case RV_SPACE: space(v, p, raw, freeze, l, r, n); break;
    case RV_CLASSIC: classic(v, p, freeze, l, r, n); break;
    case RV_BLOOM: case RV_MIDIVERB: blooms(v, algo, p, raw, freeze, l, r, n); break;
    case RV_SPRING: spring(v, p, freeze, l, r, n); break;
    case RV_INVERSE: inverse(v, p, freeze, l, r, n); break;
    case RV_TEMPLE: temple(v, p, raw, freeze, l, r, n); break;
    default: nebula(v, p, freeze, l, r, n); break;
    }
    if (era < 0.98f) {
        for (int i = 0; i < n; i++) {
            v->obw_z[0][0] += bk * (l[i] - v->obw_z[0][0]); l[i] = v->obw_z[0][0];
            v->obw_z[1][0] += bk * (r[i] - v->obw_z[1][0]); r[i] = v->obw_z[1][0];
        }
    }
    if (fabsf(width - 1.f) > 0.01f)
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (l[i] + r[i]), s = 0.5f * (l[i] - r[i]) * width;
            l[i] = m + s;
            r[i] = m - s;
        }
    /* DUCK: the tail steps back while you play and blooms in the gaps */
    if (duck > 0.001f) {
        for (int i = 0; i < n; i++) {
            v->dk_env += (lev[i] - v->dk_env) * (lev[i] > v->dk_env ? 0.01f : 0.0003f);
            float g = 1.f / (1.f + duck * 16.f * v->dk_env);
            v->dk_g += (g - v->dk_g) * (g < v->dk_g ? 0.003f : 0.0003f);
            l[i] *= v->dk_g;
            r[i] *= v->dk_g;
        }
    } else
        v->dk_g = 1.f, v->dk_env = 0.f;
}

/* ---------------------------------------------------------------- the UI side */
static void secs(char *o, int n, float s) { snprintf(o, n, s < 1.f ? "%.0f ms" : "%.1f s", s < 1.f ? s * 1000.f : s); }

static void fmt(int algo, int p, float v, const Patch *pt, char *o, int n)
{
    (void)pt;
    if (p >= NBOARD) {
        int d = p - NBOARD;
        if (d == 6) { snprintf(o, n, v < 0.01f ? "OFF" : "%.0f %%", v * 100.f); return; }
        if (d == 7) { snprintf(o, n, v < 0.01f ? "MONO" : "%.0f %%", v * 200.f); return; }
        switch (algo) {
        case RV_CONCERT: case RV_PLATE: case RV_HALO:
            if (d == 0) { snprintf(o, n, "%.0f Hz", expmap(v, 80.f, 1000.f)); return; }
            if (d == 1) { snprintf(o, n, "%.2f Hz", expmap(v, 0.1f, 4.f)); return; }
            if (algo == RV_HALO && d == 3) { snprintf(o, n, "%.0f ms", expmap(v, 0.03f, 0.2f) * 1000.f); return; }
            break;
        case RV_CHAMBER:
            if (d == 0) { snprintf(o, n, "%.0f Hz", expmap(v, 80.f, 1000.f)); return; }
            if (d == 1 || d == 3) { snprintf(o, n, "x%.2f", d == 1 ? expmap(v, 0.5f, 2.5f) : expmap(v, 0.25f, 4.f)); return; }
            break;
        case RV_SPACE:
            if (d == 0) { snprintf(o, n, "x%.2f", expmap(v, 0.25f, 4.f)); return; }
            if (d == 1) { snprintf(o, n, "x%.1f", expmap(v, 1.f, 6.f)); return; }
            break;
        case RV_CLASSIC:
            if (d == 0) { snprintf(o, n, "%.2f Hz", expmap(v, 0.1f, 2.5f)); return; }
            if (d == 2) { snprintf(o, n, v < 0.02f ? "MONO" : "%.0f %%", v * 100.f); return; }
            break;
        case RV_SPRING:
            if (d == 0) { snprintf(o, n, "%.0f Hz", expmap(v, 30.f, 400.f)); return; }
            if (d == 1) { snprintf(o, n, "x%.2f", 1.f + v * 0.5f); return; }
            if (d == 2) { snprintf(o, n, "%.1f Hz", expmap(v, 0.3f, 8.f)); return; }
            if (d == 3) { snprintf(o, n, "%d STAGES", 4 + (int)(v * (SPRING_AP - 4) + 0.5f)); return; }
            break;
        case RV_INVERSE:
            if (d == 0) { snprintf(o, n, "%d TAPS", 8 + (int)(v * (INV_TAPS - 8) + 0.5f)); return; }
            break;
        case RV_TEMPLE:
            if (d == 0) { snprintf(o, n, "+-%.0f cents", v * 0.012f * 1731.f); return; }
            break;
        }
        snprintf(o, n, "%.0f %%", v * 100.f);
        return;
    }
    if (algo == RV_MIDIVERB) {
        if (p == 0) { snprintf(o, n, pstep(v, 2) ? "BLOOM 2 (49)" : "BLOOM 1 (45)"); return; }
        if (p == 1) { snprintf(o, n, "%.0f Hz", expmap(v, 20.f, 1000.f)); return; }
        if (p == 2) { snprintf(o, n, "%.0f Hz", expmap(v, 1000.f, 16000.f)); return; }
        snprintf(o, n, "%.0f %%", v * 150.f);
        return;
    }
    if (p == 4) { secs(o, n, v * 0.5f); return; }
    if (p == 5) { snprintf(o, n, v < 0.25f ? "70s %.0f" : v < 0.5f ? "80s %.0f" : v < 0.8f ? "90s %.0f" : "NOW %.0f", v * 100.f); return; }
    switch (algo) {
    case RV_CONCERT: case RV_PLATE: case RV_HALO: {
        float lo = algo == RV_PLATE ? 0.3f : algo == RV_HALO ? 1.f : 0.4f, hi = algo == RV_PLATE ? 12.f : algo == RV_HALO ? 40.f : 30.f;
        if (p == 0) { secs(o, n, expmap(v, lo, hi)); return; }
        if (algo != RV_HALO && p == 6) { snprintf(o, n, "x%.2f", expmap(v, 0.5f, 2.5f)); return; }
        if (algo == RV_HALO && p == 7) { snprintf(o, n, "%s", SHIN[pstep(v, 5)]); return; }
    } break;
    case RV_CHAMBER:
        if (p == 0) { secs(o, n, expmap(v, 0.3f, 25.f)); return; }
        if (p == 1) { snprintf(o, n, "x%.2f", expmap(v, 0.4f, 2.5f)); return; }
        if (p == 7) { snprintf(o, n, "%s", ERN[pstep(v, 4)]); return; }
        break;
    case RV_SPACE:
        if (p == 0) { secs(o, n, expmap(v, 0.3f, 20.f)); return; }
        if (p == 6) { snprintf(o, n, "%s", SPN[pstep(v, 4)]); return; }
        break;
    case RV_CLASSIC:
        if (p == 1) { snprintf(o, n, "%.0f m2", 0.625f * exp2f(10.f * v)); return; }
        if (p == 7) { snprintf(o, n, "%.0f Hz", expmap(v, 20.f, 1000.f)); return; }
        break;
    case RV_BLOOM:
        if (p == 0) { secs(o, n, expmap(v, 1.f, 30.f)); return; }
        if (p == 1) { snprintf(o, n, "x%.2f", 0.5f + v); return; }
        if (p == 7) { snprintf(o, n, pstep(v, 2) ? "BLOOM 2" : "BLOOM 1"); return; }
        break;
    case RV_SPRING:
        if (p == 0) { secs(o, n, expmap(v, 0.5f, 8.f)); return; }
        if (p == 1) { secs(o, n, expmap(v, 0.018f, 0.07f)); return; }
        break;
    case RV_TEMPLE:
        if (p == 0) { secs(o, n, expmap(v, 0.5f, 30.f)); return; }
        if (p == 1) { int st = (int)floorf(v * 36.f + 0.5f); snprintf(o, n, "%s%d  %.0f Hz", NOTEN[st % 12], 1 + st / 12, temple_root(v)); return; }
        if (p == 7) { snprintf(o, n, "%s", CHORDN[pstep(v, 8)]); return; }
        break;
    case RV_NEBULA:
        if (p == 1) { secs(o, n, expmap(v, 0.04f, 0.5f)); return; }
        if (p == 6) { snprintf(o, n, "%.0f /s", expmap(v, 4.f, 80.f)); return; }
        break;
    case RV_INVERSE:
        if (p == 0) { secs(o, n, expmap(v, 0.08f, 0.9f)); return; }
        if (p == 1) { snprintf(o, n, v < 0.1f ? "GATED" : v > 0.9f ? "INVERSE" : "%.0f%% INV", v * 100.f); return; }
        break;
    }
    snprintf(o, n, "%.0f %%", v * 100.f);
}

const ModInfo verb_info = {
    "REVERB",
    RV_N,
    {"CONCERT", "PLATE", "CHAMBER 8X8", "SPACE", "CLASSIC", "BLOOM", "MIDIVERB", "HALO", "SPRING", "INVERSE", "TEMPLE", "NEBULA"},
    {"Lexicon-style figure-8 hall: spin and wander chorus",
     "the same tank, small, dense, bright",
     "early pattern into an 8x8 Hadamard FDN tail",
     "Space Station-style recirculating taps: metallic, fluttering",
     "16 combs + 7-tap early: ClassicReverb RE-04's structure",
     "adjustable MidiVerb II Bloom: swells instead of starting dense",
     "the MidiVerb II Bloom programs 45 / 49, exactly",
     "shimmer: every pass climbs an interval",
     "two dispersive springs: drip, wobble, tube drive",
     "non-linear: swells up (inverse) or cut off (gated)",
     "a room tuned to a chord: the tail rings in harmony",
     "granular reverb: a cloud of scattered, pitch-smeared grains"},
    {
        {"DECAY", "SIZE", "DAMP", "MOD", "PRE-DELAY", "COLOR", "BASS", "DIFFUSION", "X-OVER", "SPIN RATE", "WANDER", "TANK DIFF", "", "", "DUCK", "WIDTH"},
        {"DECAY", "SIZE", "DAMP", "MOD", "PRE-DELAY", "COLOR", "BASS", "DIFFUSION", "X-OVER", "SPIN RATE", "WANDER", "TANK DIFF", "", "", "DUCK", "WIDTH"},
        {"DECAY", "SIZE", "DAMP", "MOD", "PRE-DELAY", "COLOR", "EARLY", "PATTERN", "X-OVER", "LOW DECAY", "DIFFUSION", "WANDER RATE", "", "", "DUCK", "WIDTH"},
        {"DECAY", "SIZE", "DAMP", "MOD", "PRE-DELAY", "COLOR", "MODE", "DENSITY", "WANDER RATE", "DRIVE", "", "", "", "", "DUCK", "WIDTH"},
        {"DECAY", "ROOM", "HI DAMP", "MOD", "PRE-DELAY", "COLOR", "EARLY", "LOW CUT", "WANDER RATE", "DIFFUSER", "STEREO", "", "", "", "DUCK", "WIDTH"},
        {"DECAY", "SIZE", "DAMP", "", "PRE-DELAY", "COLOR", "DIFFUSION", "TYPE", "", "", "", "", "", "", "DUCK", "WIDTH"},
        {"TYPE", "LOW CUT", "HIGH CUT", "WIDTH", "", "", "", "", "", "", "", "", "", "", "DUCK", ""},
        {"DECAY", "SIZE", "DAMP", "MOD", "PRE-DELAY", "COLOR", "SHIMMER", "INTERVAL", "X-OVER", "SPIN RATE", "DIFFUSION", "WINDOW", "", "", "DUCK", "WIDTH"},
        {"DECAY", "TENSION", "DAMP", "WOBBLE", "PRE-DELAY", "COLOR", "DRIP", "DRIVE", "LOW CUT", "PAIR", "WOBBLE RATE", "CHIRP", "", "", "DUCK", "WIDTH"},
        {"TIME", "SHAPE", "DAMP", "", "PRE-DELAY", "COLOR", "DENSITY", "SPREAD", "TAPS", "JITTER", "", "", "", "", "DUCK", "WIDTH"},
        {"DECAY", "ROOT", "DAMP", "DRIFT", "PRE-DELAY", "COLOR", "DIFFUSION", "CHORD", "SPREAD", "", "", "", "", "", "DUCK", "WIDTH"},
        {"DECAY", "SIZE", "DAMP", "SCATTER", "PRE-DELAY", "COLOR", "DENSITY", "OCTAVES", "REVERSE", "DIFFUSION", "", "", "", "", "DUCK", "WIDTH"},
    },
    {
        {0.6f, 0.55f, 0.45f, 0.4f, 0.04f, 0.6f, 0.45f, 0.7f, 0.4511309f, 0.5956347f, 0.4f, 0.6f, 0.f, 0.f, 0.f, 0.5f},
        {0.5f, 0.5f, 0.3f, 0.2f, 0.02f, 0.75f, 0.4f, 0.8f, 0.4511309f, 0.5956347f, 0.4f, 0.6f, 0.f, 0.f, 0.f, 0.5f},
        {0.55f, 0.5f, 0.4f, 0.3f, 0.03f, 0.85f, 0.5f, 0.1f, 0.3627827f, 0.5936926f, 0.5f, 0.5f, 0.f, 0.f, 0.f, 0.5f},
        {0.5f, 0.5f, 0.45f, 0.35f, 0.f, 0.45f, 0.f, 0.4f, 0.5f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.5f},
        {0.65f, 0.55f, 0.35f, 0.15f, 0.04f, 0.7f, 0.5f, 0.2f, 0.5f, 0.75f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.5f},
        {0.6f, 0.5f, 0.5f, 0.f, 0.f, 0.5f, 0.5f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.5f},
        {0.f, 0.f, 1.f, 0.67f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.5f},
        {0.55f, 0.6f, 0.4f, 0.45f, 0.05f, 0.85f, 0.45f, 0.f, 0.4511309f, 0.5956347f, 0.7f, 0.6346319f, 0.f, 0.f, 0.f, 0.5f},
        {0.45f, 0.4f, 0.4f, 0.3f, 0.f, 0.4f, 0.5f, 0.2f, 0.4241309f, 0.42f, 0.6203548f, 1.f, 0.f, 0.f, 0.f, 0.5f},
        {0.45f, 0.9f, 0.3f, 0.f, 0.f, 0.6f, 0.7f, 0.6f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.5f},
        {0.55f, 0.4f, 0.45f, 0.3f, 0.02f, 0.85f, 0.6f, 0.15f, 0.3f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.5f},
        {0.6f, 0.5f, 0.4f, 0.4f, 0.03f, 0.8f, 0.5f, 0.2f, 0.25f, 0.6f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.5f},
    },
    0.5f,
    fmt,
};
