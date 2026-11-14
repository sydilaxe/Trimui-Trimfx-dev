/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TONE: mastering / tonal shaping and character at the head of the chain.
 *   LONDON, PUNCH, CLEAN, LACQUER  SP-1 Master's engine (src/master, MIT): a
 *       tone curve -> glue compressor -> colour stage -> drive into a
 *       look-ahead limiter, with a bit crusher and resonant filter before
 *   CASSETTE  saturation, wow and flutter, age, dropouts, hiss
 *   SAMPLER   an old 12-bit sampler: few bits, a low rate with no
 *             anti-aliasing (it folds), a jittery clock, a resonant filter
 *   TUBE      an asymmetric triode stage: warmth, air, mids, power sag
 *   FOLD      a wavefolder with rectifier and sub-octave: from glow to grind */
#include <stdio.h>

#include "modules.h"

/* the parameter slots, ordered so the first four (on the sticks in FOCUS
 * mode) are the ones that move the sound most */
enum { T_TILT, T_FILTER, T_COLOUR, T_CRUSH, T_TONE, T_GLUE, T_LOUD, T_WIDTH };

int tone_init(Tone *t)
{
    memset(t, 0, sizeof *t);
    ms_init(&t->ms, (float)TFX_SR);
    t->rng.s = 0x51ab1e5u;
    for (int c = 0; c < 2; c++)
        if (dly_init(&t->wow[c], TFX_SR / 50)) return -1;
    for (int k = 0; k < 3; k++) t->bqp[k] = -1.f;
    return 0;
}
void tone_free(Tone *t) { for (int c = 0; c < 2; c++) dly_free(&t->wow[c]); }
void tone_reset(Tone *t, int algo)
{
    ms_init(&t->ms, (float)TFX_SR);
    if (algo < TN_CASSETTE) ms_set_chain(&t->ms, algo);
    t->chain = algo < TN_CASSETTE ? algo : 0;
    for (int c = 0; c < 2; c++) {
        dly_clear(&t->wow[c]);
        t->lp1[c] = t->lp2[c] = t->tilt[c] = t->hold[c] = t->pink[c] = t->dc[c] = t->sub[c] = t->subs[c] = t->subz[c] = 0.f;
        t->sv[c].ic1 = t->sv[c].ic2 = 0.f;
        for (int k = 0; k < 3; k++) t->bq[c][k].z1 = t->bq[c][k].z2 = 0.f;
    }
    t->env = 0.f; t->drop = 1.f; t->hph = 0.f;
}

/* ---------------------------------------------------------------- the character algorithms */
/* tilt around ~700 Hz, -1..1 (dark .. bright), +-6 dB */
static inline float tilt(Tone *t, int c, float x, float amt)
{
    t->tilt[c] += 0.09f * (x - t->tilt[c]);
    float lo = t->tilt[c], hi = x - lo;
    return lo * (1.f - 0.5f * amt) + hi * (1.f + amt);
}
static inline void width(float *l, float *r, float w)
{
    float m = 0.5f * (*l + *r), s = 0.5f * (*l - *r) * w;
    *l = m + s; *r = m - s;
}
/* an asymmetric saturator: bias moves the curve off centre (even harmonics); DC removed after */
static inline float asym(float x, float bias) { return softclip(x + bias) - softclip(bias); }

static void cassette(Tone *t, const float *p, float *l, float *r, int n)
{
    float drive = 1.f + p[0] * 5.f, wow = p[1], dk = onepole_coef(expmap(1.f - p[2], 3500.f, 16000.f));
    float drop = p[3], tl = p[4] * 2.f - 1.f, hiss = p[5] * p[5] * 0.02f, bias = p[6] * 0.6f, wd = p[7] * 1.4f;
    float comp = 1.f / (0.55f + 0.45f * drive);
    for (int i = 0; i < n; i++) {
        t->wph += 0.55f / TFX_SR; t->wph -= floorf(t->wph);
        t->fph += 9.3f / TFX_SR; t->fph -= floorf(t->fph);
        float mo = wow * (0.0012f * (0.7f * lfo_sin(t->wph) + 0.3f * wander_tick(&t->ww, &t->rng, 1.1f)) + 0.00018f * lfo_sin(t->fph));
        /* dropouts: now and then the level sags, as worn tape leaves the head */
        float dv = wander_tick(&t->dw, &t->rng, 3.f);
        float target = dv > 0.75f - 0.6f * drop && drop > 0.01f ? 0.25f + 0.5f * (1.f - drop) : 1.f;
        t->drop += (target - t->drop) * (target < t->drop ? 0.01f : 0.0015f);
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            dly_push(&t->wow[c], x[c]);
            float y = dly_herm(&t->wow[c], 0.004f * TFX_SR + mo * TFX_SR);
            y = asym(y * drive, bias) * comp;
            t->dc[c] += 0.0005f * (y - t->dc[c]);
            y -= t->dc[c];
            t->lp1[c] += dk * (y - t->lp1[c]);
            t->lp2[c] += dk * (t->lp1[c] - t->lp2[c]);
            y = tilt(t, c, t->lp2[c], tl) * t->drop;
            t->pink[c] = 0.97f * t->pink[c] + 0.03f * rng_bi(&t->rng);
            x[c] = y + hiss * t->pink[c] * 4.f;
        }
        width(&x[0], &x[1], wd);
        l[i] = x[0]; r[i] = x[1];
    }
}

static void sampler(Tone *t, const float *p, float *l, float *r, int n)
{
    float bits = expmap(1.f - p[0], 4.f, 16.f), q = exp2f(1.f - bits);
    float rate = expmap(1.f - p[1], 3000.f, 32000.f) / TFX_SR;
    float g = svf_g(expmap(p[2], 300.f, 18000.f)), k = 1.f / expmap(p[3], 0.6f, 12.f);
    float drive = 1.f + p[4] * 6.f, jit = p[5] * 0.5f, tl = p[6] * 2.f - 1.f, wd = p[7] * 1.4f;
    for (int i = 0; i < n; i++) {
        /* the clock: a sample is taken (and held) as the phase wraps; jitter wobbles it */
        t->hph += rate * (1.f + jit * t->jit);
        float x[2] = {l[i], r[i]};
        if (t->hph >= 1.f) {
            t->hph -= floorf(t->hph);
            t->jit = rng_bi(&t->rng);
            for (int c = 0; c < 2; c++) {
                float y = softclip(x[c] * drive) / (0.6f + 0.4f * drive);
                t->hold[c] = q * floorf(y / q + 0.5f);
            }
        }
        for (int c = 0; c < 2; c++) {
            float y = svf_run(&t->sv[c], t->hold[c], g, k).lp;
            x[c] = tilt(t, c, softclip(y), tl);
        }
        width(&x[0], &x[1], wd);
        l[i] = x[0]; r[i] = x[1];
    }
}

static void tube(Tone *t, const float *p, float *l, float *r, int n)
{
    float drive = expmap(p[0], 1.f, 20.f), warm = p[1], air = p[2] * 2.f - 1.f, sag = p[3], bias = 0.05f + p[4] * 0.5f;
    float mids = p[5] * 2.f - 1.f, tl = p[6] * 2.f - 1.f, wd = p[7] * 1.4f;
    float want[3] = {warm * 7.f, air * 6.f, mids * 8.f};
    for (int k = 0; k < 3; k++)
        if (want[k] != t->bqp[k]) {
            t->bqp[k] = want[k];
            for (int c = 0; c < 2; c++)
                bq_set(&t->bq[c][k], k == 0 ? BQ_LSHELF : k == 1 ? BQ_HSHELF : BQ_PEAK, k == 0 ? 120.f : k == 1 ? 9000.f : 900.f,
                       k == 2 ? 0.8f : 0.7071f, want[k]);
        }
    float comp = 1.f / sqrtf(drive);
    for (int i = 0; i < n; i++) {
        float x[2] = {l[i], r[i]};
        float lev = fmaxf(fabsf(x[0]), fabsf(x[1])) * drive;
        t->env += (lev - t->env) * (lev > t->env ? 0.01f : 0.0003f);
        /* power sag: the supply droops under load, the stage softens and breathes */
        float sg = 1.f / (1.f + sag * 2.f * t->env);
        for (int c = 0; c < 2; c++) {
            float y = bq_run(&t->bq[c][0], x[c]);
            y = asym(y * drive * sg, bias + 0.4f * warm * 0.3f);
            t->dc[c] += 0.0005f * (y - t->dc[c]);
            y = (y - t->dc[c]) * comp * 1.4f;
            y = bq_run(&t->bq[c][2], bq_run(&t->bq[c][1], y));
            x[c] = tilt(t, c, y, tl);
        }
        width(&x[0], &x[1], wd);
        l[i] = x[0]; r[i] = x[1];
    }
}

static void fold(Tone *t, const float *p, float *l, float *r, int n)
{
    float g = expmap(p[0], 1.f, 14.f), sym = (p[1] * 2.f - 1.f) * 0.6f, dk = onepole_coef(expmap(p[2], 500.f, 18000.f));
    float blend = p[3], rect = p[4], sub = p[5], tl = p[6] * 2.f - 1.f, wd = p[7] * 1.4f;
    for (int i = 0; i < n; i++) {
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float in = x[c];
            /* a sine folder: past its peak the wave folds back on itself */
            float y = lfo_sin((in * g + sym) * 0.25f);
            y = lerpf(y, fabsf(y) * 2.f - 0.6f, rect); /* rectify: an octave up */
            /* sub: a flip-flop on the input's rising zero crossings, an octave down, at its level */
            if (t->subz[c] <= 0.f && in > 0.f) t->sub[c] = t->sub[c] > 0.f ? -1.f : 1.f;
            t->subz[c] = in;
            t->subs[c] += 0.003f * (fabsf(in) - t->subs[c]);
            y += sub * t->sub[c] * t->subs[c] * 2.5f;
            t->dc[c] += 0.0005f * (y - t->dc[c]);
            y -= t->dc[c];
            t->lp1[c] += dk * (y - t->lp1[c]);
            y = lerpf(t->lp1[c] * 0.6f, in, blend * 0.5f);
            x[c] = tilt(t, c, y, tl);
        }
        width(&x[0], &x[1], wd);
        l[i] = x[0]; r[i] = x[1];
    }
}
int tone_latency(void) { return LIM_LA; }
float tone_gr_db(Tone *t)
{
    float g = ms_gain_reduction_db(&t->ms);
    ms_meter_fall(&t->ms, 0.7f);
    return g;
}

void tone_process(Tone *t, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    (void)raw, (void)pt;
    switch (algo) {
    case TN_CASSETTE: cassette(t, p, l, r, n); return;
    case TN_SAMPLER: sampler(t, p, l, r, n); return;
    case TN_TUBE: tube(t, p, l, r, n); return;
    case TN_FOLD: fold(t, p, l, r, n); return;
    }
    ms_engine *e = &t->ms;
    if (algo != t->chain) ms_set_chain(e, algo), t->chain = algo;
    ms_set_fader(e, MS_TONE, p[T_TONE]);
    ms_set_fader(e, MS_GLUE, p[T_GLUE]);
    ms_set_fader(e, MS_COLOUR, p[T_COLOUR]);
    ms_set_fader(e, MS_LOUD, p[T_LOUD]);
    ms_set_param2(e, MS_TONE, p[T_TILT]);
    ms_set_param2(e, MS_GLUE, 0.5f);
    ms_set_param2(e, MS_COLOUR, p[T_WIDTH]);
    ms_set_param2(e, MS_LOUD, 0.5f);
    /* CRUSH: one control for the whole crusher: how much, how few bits, how slow */
    float c = p[T_CRUSH];
    ms_set_crush(e, MS_CR_AMOUNT, c < 0.02f ? 0.f : 0.25f + 0.75f * c);
    ms_set_crush(e, MS_CR_BITS, 0.25f + 0.6f * c);
    ms_set_crush(e, MS_CR_RATE, 0.15f + 0.6f * c * c);
    ms_set_crush(e, MS_CR_FILTER, p[T_FILTER]);
    ms_process(e, l, r, n);
}

static void fmt(int algo, int p, float v, const Patch *pt, char *o, int n)
{
    (void)pt;
    if (algo >= TN_CASSETTE) {
        float b = v * 2.f - 1.f;
        if (p == 7) { snprintf(o, n, v < 0.03f ? "MONO" : "%.0f %%", v * 140.f); return; }
        switch (algo) {
        case TN_CASSETTE:
            if (p == 2) { snprintf(o, n, "%.1f kHz", expmap(1.f - v, 3500.f, 16000.f) / 1000.f); return; }
            if (p == 4) { snprintf(o, n, "%+.0f", b * 100.f); return; }
            break;
        case TN_SAMPLER:
            if (p == 0) { snprintf(o, n, "%.1f bit", expmap(1.f - v, 4.f, 16.f)); return; }
            if (p == 1) { snprintf(o, n, "%.1f kHz", expmap(1.f - v, 3000.f, 32000.f) / 1000.f); return; }
            if (p == 2) { snprintf(o, n, "%.0f Hz", expmap(v, 300.f, 18000.f)); return; }
            if (p == 6) { snprintf(o, n, "%+.0f", b * 100.f); return; }
            break;
        case TN_TUBE:
            if (p == 0) { snprintf(o, n, "x%.1f", expmap(v, 1.f, 20.f)); return; }
            if (p == 1) { snprintf(o, n, "+%.1f dB", v * 7.f); return; }
            if (p == 2) { snprintf(o, n, "%+.1f dB", b * 6.f); return; }
            if (p == 5) { snprintf(o, n, "%+.1f dB", b * 8.f); return; }
            if (p == 6) { snprintf(o, n, "%+.0f", b * 100.f); return; }
            break;
        case TN_FOLD:
            if (p == 0) { snprintf(o, n, "x%.1f", expmap(v, 1.f, 14.f)); return; }
            if (p == 1) { snprintf(o, n, "%+.0f", b * 100.f); return; }
            if (p == 2) { snprintf(o, n, "%.0f Hz", expmap(v, 500.f, 18000.f)); return; }
            if (p == 6) { snprintf(o, n, "%+.0f", b * 100.f); return; }
            break;
        }
        snprintf(o, n, "%.0f %%", v * 100.f);
        return;
    }
    switch (p) {
    case T_TILT: snprintf(o, n, "%+.1f dB", (v - 0.5f) * 8.f); break;
    case T_FILTER:
        if (v > 0.4f && v < 0.6f) snprintf(o, n, "FLAT");
        else if (v <= 0.4f) snprintf(o, n, "LP %.0f Hz", expmap(v / 0.4f, 60.f, 16000.f));
        else snprintf(o, n, "HP %.0f Hz", expmap((v - 0.6f) / 0.4f, 20.f, 6000.f));
        break;
    case T_WIDTH: snprintf(o, n, v < 0.03f ? "MONO" : "%.0f %%", v < 0.5f ? v * 200.f : 100.f + (v - 0.5f) * 120.f); break;
    case T_LOUD: snprintf(o, n, "+%.1f dB", v * 12.f); break;
    default: snprintf(o, n, "%.0f %%", v * 100.f); break;
    }
}

const ModInfo tone_info = {
    "TONE",
    TN_N,
    {"LONDON", "PUNCH", "CLEAN", "LACQUER", "CASSETTE", "SAMPLER", "TUBE", "FOLD"},
    {"60s studio: warm shelves, slow tube glue, transformer",
     "NY punch: big lows, fast bus comp, parallel squash",
     "modern: tilt, 3-band glue, saturated air band",
     "cutting room: warm curve, mono bass, tamed top",
     "worn tape: saturation, wow, age, dropouts, hiss",
     "old 12-bit sampler: folding rate, few bits, resonant filter",
     "triode stage: asymmetric drive, warmth, air, power sag",
     "wavefolder: folds, rectifies, sub-octave, glow to grind"},
    {
        {"TILT", "FILTER", "COLOUR", "CRUSH", "TONE", "GLUE", "LOUD", "WIDTH"},
        {"TILT", "FILTER", "COLOUR", "CRUSH", "TONE", "GLUE", "LOUD", "WIDTH"},
        {"TILT", "FILTER", "COLOUR", "CRUSH", "TONE", "GLUE", "LOUD", "WIDTH"},
        {"TILT", "FILTER", "COLOUR", "CRUSH", "TONE", "GLUE", "LOUD", "WIDTH"},
        {"SATURATE", "WOW", "AGE", "DROPOUT", "TILT", "HISS", "BIAS", "WIDTH"},
        {"BITS", "RATE", "FILTER", "RESO", "DRIVE", "JITTER", "TILT", "WIDTH"},
        {"DRIVE", "WARMTH", "AIR", "SAG", "BIAS", "MIDS", "TILT", "WIDTH"},
        {"FOLD", "SYMMETRY", "TONE", "BLEND", "RECTIFY", "SUB", "TILT", "WIDTH"},
    },
    {
        {0.5f, 0.5f, 0.35f, 0.f, 0.5f, 0.3f, 0.15f, 0.5f},
        {0.5f, 0.5f, 0.3f, 0.f, 0.5f, 0.35f, 0.2f, 0.5f},
        {0.5f, 0.5f, 0.3f, 0.f, 0.5f, 0.3f, 0.15f, 0.55f},
        {0.5f, 0.5f, 0.3f, 0.f, 0.5f, 0.3f, 0.15f, 0.5f},
        {0.4f, 0.35f, 0.35f, 0.1f, 0.5f, 0.15f, 0.3f, 0.6f},
        {0.45f, 0.4f, 0.7f, 0.25f, 0.2f, 0.15f, 0.5f, 0.7f},
        {0.35f, 0.4f, 0.55f, 0.3f, 0.3f, 0.5f, 0.5f, 0.7f},
        {0.3f, 0.5f, 0.6f, 0.3f, 0.f, 0.f, 0.5f, 0.7f},
    },
    1.f,
    fmt,
};
