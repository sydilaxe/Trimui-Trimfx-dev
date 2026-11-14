/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TrimFX: small DSP building blocks shared by the modules. Everything is
 * float, 48 kHz stereo, header-only and allocation-free once made. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TFX_SR 48000
#define TFX_PI 3.14159265358979f
#define TFX_2PI 6.28318530717959f

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float db2lin(float db) { return expf(db * 0.11512925f); }
static inline float lin2db(float g) { return g > 1e-9f ? 20.f * log10f(g) : -180.f; }
/* exponential map 0..1 -> lo..hi */
static inline float expmap(float t, float lo, float hi) { return lo * expf(logf(hi / lo) * t); }
/* soft clip: a rational tanh, unity at 0, ~1 at |x| = 3 */
static inline float softclip(float x)
{
    x = clampf(x, -3.f, 3.f);
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}
/* a cheap sine for LFOs, phase 0..1 (max error ~0.001) */
static inline float lfo_sin(float ph)
{
    float x = ph - floorf(ph);
    x = x < 0.5f ? x : x - 1.f; /* -0.5..0.5 */
    float y = 8.f * x - 16.f * x * fabsf(x);
    return 0.225f * (y * fabsf(y) - y) + y;
}
/* one-pole low-pass coefficient for a cutoff in Hz */
static inline float onepole_coef(float hz) { return 1.f - expf(-TFX_2PI * hz / TFX_SR); }

/* ---- random: xorshift32 and a smooth random walk */
typedef struct { uint32_t s; } Rng;
static inline uint32_t rng_u32(Rng *r)
{
    uint32_t x = r->s ? r->s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->s = x;
}
static inline float rng_uni(Rng *r) { return (float)(rng_u32(r) >> 8) * (1.f / 16777216.f); } /* 0..1 */
static inline float rng_bi(Rng *r) { return rng_uni(r) * 2.f - 1.f; }                          /* -1..1 */

/* a smooth random value: a new target every 1/rate s, glided with a cosine */
typedef struct { float a, b, ph; } Wander;
static inline float wander_tick(Wander *w, Rng *r, float rate_hz)
{
    w->ph += rate_hz * (1.f / TFX_SR);
    if (w->ph >= 1.f) { w->ph -= floorf(w->ph); w->a = w->b; w->b = rng_bi(r); }
    float t = 0.5f - 0.5f * lfo_sin(w->ph * 0.5f + 0.25f); /* cos ramp 0..1 */
    return w->a + (w->b - w->a) * t;
}

/* ---- delay line (power-of-two length, mono) */
typedef struct { float *b; uint32_t mask, w; } Dly;
static inline int dly_init(Dly *d, int min_len)
{
    uint32_t n = 1;
    while (n < (uint32_t)min_len + 4) n <<= 1;
    d->b = calloc(n, sizeof(float));
    d->mask = n - 1; d->w = 0;
    return d->b ? 0 : -1;
}
static inline void dly_free(Dly *d) { free(d->b); d->b = NULL; }
static inline void dly_clear(Dly *d) { if (d->b) memset(d->b, 0, sizeof(float) * (d->mask + 1)); }
static inline void dly_push(Dly *d, float x) { d->b[d->w] = x; d->w = (d->w + 1) & d->mask; }
/* d samples back (1 = the last written) */
static inline float dly_tap(const Dly *d, uint32_t n) { return d->b[(d->w - n) & d->mask]; }
static inline float dly_lin(const Dly *d, float n)
{
    if (n < 1.f) n = 1.f;
    uint32_t i = (uint32_t)n; float f = n - (float)i;
    float a = d->b[(d->w - i) & d->mask], b = d->b[(d->w - i - 1) & d->mask];
    return a + (b - a) * f;
}
/* 4-point Hermite, for pitch-moving reads */
static inline float dly_herm(const Dly *d, float n)
{
    if (n < 2.f) n = 2.f;
    uint32_t i = (uint32_t)n; float f = n - (float)i;
    float xm1 = d->b[(d->w - i + 1) & d->mask], x0 = d->b[(d->w - i) & d->mask];
    float x1 = d->b[(d->w - i - 1) & d->mask], x2 = d->b[(d->w - i - 2) & d->mask];
    float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}
/* absolute read at a position counted from the buffer start (for granular / reverse) */
static inline float dly_at(const Dly *d, float pos)
{
    float fl = floorf(pos);
    uint32_t i = (uint32_t)(int32_t)fl; float f = pos - fl;
    float a = d->b[i & d->mask], b = d->b[(i + 1) & d->mask];
    return a + (b - a) * f;
}

/* ---- Schroeder all-pass on its own delay line, with an optionally moving length */
typedef struct { Dly d; float len; } Ap;
static inline int ap_init(Ap *a, int max_len, float len) { a->len = len; return dly_init(&a->d, max_len + 8); }
static inline float ap_run(Ap *a, float x, float g)
{
    float z = dly_lin(&a->d, a->len);
    float w = x + g * z;
    dly_push(&a->d, w);
    return z - g * w;
}
static inline float ap_run_mod(Ap *a, float x, float g, float len)
{
    float z = dly_lin(&a->d, len);
    float w = x + g * z;
    dly_push(&a->d, w);
    return z - g * w;
}

/* ---- biquad (RBJ), transposed direct form II */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Bq;
enum { BQ_LP, BQ_HP, BQ_BP, BQ_PEAK, BQ_LSHELF, BQ_HSHELF };
static inline void bq_set(Bq *q, int type, float hz, float Q, float gain_db)
{
    float w = TFX_2PI * clampf(hz, 10.f, TFX_SR * 0.49f) / TFX_SR, cw = cosf(w), sw = sinf(w);
    float al = sw / (2.f * Q), A = powf(10.f, gain_db / 40.f), b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case BQ_LP: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case BQ_HP: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case BQ_BP: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case BQ_PEAK: b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
    case BQ_LSHELF: {
        float s = 2 * sqrtf(A) * al;
        b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
    } break;
    case BQ_HSHELF: {
        float s = 2 * sqrtf(A) * al;
        b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
    } break;
    }
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0; q->a1 = a1 / a0; q->a2 = a2 / a0;
}
static inline float bq_run(Bq *q, float x)
{
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* ---- state-variable filter (Simper / Cytomic), cheap to sweep per sample */
typedef struct { float ic1, ic2; } Svf;
typedef struct { float lp, bp, hp; } SvfOut;
static inline SvfOut svf_run(Svf *s, float x, float g /* tan(pi f / sr) */, float k /* 1/Q */)
{
    float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - s->ic2;
    float v1 = a1 * s->ic1 + a2 * v3;
    float v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2.f * v1 - s->ic1;
    s->ic2 = 2.f * v2 - s->ic2;
    SvfOut o = {v2, v1, x - k * v1 - v2};
    return o;
}
static inline float svf_g(float hz) { return tanf(TFX_PI * clampf(hz, 10.f, TFX_SR * 0.45f) / TFX_SR); }
/* a fast tan(pi f/sr) for per-sample sweeps below ~12 kHz */
static inline float svf_g_fast(float hz)
{
    float x = TFX_PI * clampf(hz, 10.f, 14000.f) / TFX_SR;
    float x2 = x * x;
    return x * (1.f + x2 * (0.333333f + x2 * 0.133333f));
}

/* ---- a parameter glide: one-pole toward a target, per block */
static inline float glide(float cur, float target, float k) { return cur + (target - cur) * k; }

/* equal-power dry/wet: mix 0 = dry, 1 = all wet (no dry at all) */
static inline void mix_gains(float mix, float *dry, float *wet)
{
    mix = clampf(mix, 0.f, 1.f);
    *dry = cosf(mix * TFX_PI * 0.5f);
    *wet = sinf(mix * TFX_PI * 0.5f);
    if (mix >= 0.999f) *dry = 0.f, *wet = 1.f;
    if (mix <= 0.001f) *dry = 1.f, *wet = 0.f;
}
