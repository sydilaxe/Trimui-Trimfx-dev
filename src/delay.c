/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DELAY: ten delays sharing 8 s of stereo memory.
 *   GRANULAR  a grain cloud read out of the delay: pitch, density, size,
 *             spray, reverse grains and octave sparkle, fed back into itself
 *   RHYTHM    a tempo-synced multi-tap pattern (echo, ping-pong, triplets,
 *             gallop, clave, euclid, cascade, scatter) with swing, tone,
 *             wobble and a diffusing feedback path
 *   REVERSO   forward and reverse heads: blend them, and since each repeat
 *             passes through the reverse head again, the repeats flip
 *             direction; smear, drift and auto-swell for ambient washes
 *   TAPE      a three-head tape echo (heads at 1/3, 2/3 and the whole
 *             time): wow, flutter, age, drive; feedback past unity is
 *             caught by the tape's saturation
 *   SHIMMER   each repeat climbs (or falls) an interval: a pitch shifter in
 *             the feedback, diffused
 *   CLOUD     four cross-coupled, wandering delays behind a diffuser: echoes
 *             that bloom into a haze, ducking out of the way as you play
 *   STUTTER   a beat-repeat: on the tempo grid it may catch a slice and
 *             repeat it, rising or falling in pitch, sometimes backwards
 *   DUB       a dub echo through a resonant filter that morphs low-pass ->
 *             band-pass -> high-pass, driven past unity into a runaway
 *   PRISM     the repeats split into four bands, each with its own time: the
 *             lows come back late and the highs early (or the other way), so
 *             every echo smears apart into its colours
 *   ERODE     a digital memory that is failing: each pass is sampled slower,
 *             with fewer bits, and drops out now and then, so the repeats
 *             crumble into dust as they go round
 * FREEZE (from the patch) closes the input and holds what is in memory. */
#include <stdio.h>

#include "modules.h"

#define DMEM (8 * TFX_SR)

int delay_init(Delay *d)
{
    memset(d, 0, sizeof *d);
    d->rng.s = 0xdecafbadu;
    for (int k = 0; k < 4; k++)
        if (dly_init(&d->cl[k], 2 * TFX_SR)) return -1;
    for (int c = 0; c < 2; c++) {
        if (dly_init(&d->psh[c], TFX_SR / 8)) return -1;
        if (ap_init(&d->cin[c][0], 1200, 389.f + c * 37) || ap_init(&d->cin[c][1], 2400, 1013.f + c * 53)) return -1;
        if (dly_init(&d->b[c], DMEM)) return -1;
        if (ap_init(&d->diff[c][0], 600, 337.f + c * 23) || ap_init(&d->diff[c][1], 900, 541.f + c * 31)) return -1;
        for (int k = 0; k < 3; k++)
            if (ap_init(&d->rdiff[c][k], 2400, (float)(419 + k * 557 + c * 41))) return -1;
    }
    return 0;
}

void delay_free(Delay *d)
{
    for (int k = 0; k < 4; k++) dly_free(&d->cl[k]);
    for (int c = 0; c < 2; c++) {
        dly_free(&d->psh[c]); dly_free(&d->cin[c][0].d); dly_free(&d->cin[c][1].d);
        dly_free(&d->b[c]);
        dly_free(&d->diff[c][0].d); dly_free(&d->diff[c][1].d);
        for (int k = 0; k < 3; k++) dly_free(&d->rdiff[c][k].d);
    }
}

void delay_reset(Delay *d, int algo)
{
    (void)algo;
    for (int k = 0; k < 4; k++) dly_clear(&d->cl[k]), d->clp[k] = 0.f;
    d->cduck = 1.f;
    d->st_on = 0; d->st_clock = 0.f;
    for (int c = 0; c < 2; c++) {
        dly_clear(&d->psh[c]); dly_clear(&d->cin[c][0].d); dly_clear(&d->cin[c][1].d);
        d->dsv[c].ic1 = d->dsv[c].ic2 = d->dfb[c] = 0.f;
        dly_clear(&d->b[c]);
        dly_clear(&d->diff[c][0].d); dly_clear(&d->diff[c][1].d);
        for (int k = 0; k < 3; k++) dly_clear(&d->rdiff[c][k].d);
        d->lp[c] = d->hp[c] = d->rc[c] = d->tsat[c] = 0.f;
    }
    memset(d->g, 0, sizeof d->g);
    d->time_s = 0.f;
    d->gnext = 0.f;
    d->env_f = d->env_s = 0.f;
    d->swell = 1.f;
    memset(d->psv, 0, sizeof d->psv);
    for (int k = 0; k < 4; k++) d->ptime[k] = 0.f;
    d->eh[0] = d->eh[1] = d->eph = d->ehiss[0] = d->ehiss[1] = 0.f;
    d->egate = d->etgt = 1.f;
    d->gon = 0;
}

/* the tempo divisions, in quarter notes */
static const float DIV[12] = {0.125f, 1.f / 6, 0.25f, 1.f / 3, 0.375f, 0.5f, 2.f / 3, 0.75f, 1.f, 4.f / 3, 1.5f, 2.f};
static const char *const DIVN[12] = {"1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T", "1/4.", "1/2"};
/* DEEP SYNC: FREE (the TIME knob), else one of the divisions */
static float synced(float raw, float bpm, float free_s)
{
    int s = pstep(raw, 13);
    return s ? 60.f / clampf(bpm, 40.f, 240.f) * DIV[s - 1] : free_s;
}

/* ---------------------------------------------------------------- GRANULAR */
static const float SEMI[10] = {-24, -12, -7, -5, 0, 5, 7, 12, 19, 24};

static void granular(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, DUCK, ENVELOPE (smooth bell .. flat with short edges), FB TONE, DETUNE (a random
     * pitch each grain) */
    float env = p[10], fbk = onepole_coef(expmap(p[11], 500.f, 18000.f)), detune = p[12] * 0.5f;
    float time = synced(raw[8], pt->bpm, expmap(p[0], 0.01f, 3.f)) * TFX_SR, fb = p[1] * 0.95f;
    float ratio0 = exp2f(SEMI[pstep(raw[2], 10)] / 12.f);
    float dens = expmap(p[3], 1.f, 60.f), size = expmap(p[4], 0.015f, 0.6f) * TFX_SR;
    float spray = p[5], rev = p[6], sparkle = p[7];
    int freeze = pt->freeze_dly;
    float over = dens * size / TFX_SR, norm = 1.8f / sqrtf(over > 1.f ? over : 1.f);
    const float N = (float)(d->b[0].mask + 1);
    for (int i = 0; i < n; i++) {
        /* a new grain */
        d->gnext -= 1.f;
        if (d->gnext <= 0.f) {
            d->gnext += TFX_SR / dens * (0.5f + rng_uni(&d->rng));
            for (int k = 0; k < GRAINS; k++) {
                Grain *g = &d->g[k];
                if (g->on) continue;
                float ratio = ratio0;
                if (rng_uni(&d->rng) < sparkle * 0.5f) ratio *= rng_uni(&d->rng) < 0.5f ? 2.f : 0.5f;
                if (detune > 0.f) ratio *= exp2f(detune * rng_bi(&d->rng) / 12.f);
                g->len = (int)(size * (0.75f + 0.5f * rng_uni(&d->rng)));
                if (g->len < 64) g->len = 64;
                float back = time + spray * rng_uni(&d->rng) * (time > 0.5f * TFX_SR ? time : 0.5f * TFX_SR);
                int reverse = rng_uni(&d->rng) < rev;
                if (!reverse && ratio > 1.f) back += (ratio - 1.f) * g->len; /* never overtake the write head */
                if (back > N - 4.f * TFX_SR / 10.f) back = N - 4.f * TFX_SR / 10.f;
                float pos = (float)d->b[0].w - back;
                while (pos < 0.f) pos += N;
                g->pos = pos;
                g->rate = reverse ? -ratio : ratio;
                g->age = 0;
                float pan = clampf(rng_bi(&d->rng) * (0.25f + 0.75f * sparkle), -1.f, 1.f);
                g->gl = sqrtf(0.5f * (1.f - pan)) * norm;
                g->gr = sqrtf(0.5f * (1.f + pan)) * norm;
                g->on = 1;
                break;
            }
        }
        float ol = 0.f, orr = 0.f;
        for (int k = 0; k < GRAINS; k++) {
            Grain *g = &d->g[k];
            if (!g->on) continue;
            float w = 0.5f - 0.5f * lfo_sin((float)g->age / (float)g->len + 0.25f);
            if (env > 0.f) w = lerpf(w, fminf(w * 6.f, 1.f), env);
            ol += w * g->gl * dly_at(&d->b[0], g->pos);
            orr += w * g->gr * dly_at(&d->b[1], g->pos);
            g->pos += g->rate;
            if (g->pos >= N) g->pos -= N;
            if (g->pos < 0.f) g->pos += N;
            if (++g->age >= g->len) g->on = 0;
        }
        if (!freeze) {
            /* the cloud back into the memory: darkened a little, kept from running away */
            d->lp[0] += fbk * (ol - d->lp[0]);
            d->lp[1] += fbk * (orr - d->lp[1]);
            dly_push(&d->b[0], l[i] + fb * softclip(d->lp[0]));
            dly_push(&d->b[1], r[i] + fb * softclip(d->lp[1]));
        }
        l[i] = ol;
        r[i] = orr;
    }
}

/* ---------------------------------------------------------------- RHYTHM */
typedef struct { int len, n; float step[8], gain[8], pan[8]; } Pattern;
static const Pattern PAT[8] = {
    {1, 1, {1}, {1}, {0}},
    {2, 2, {1, 2}, {1, 0.85f}, {-1, 1}},
    {3, 3, {1, 2, 3}, {0.7f, 0.55f, 1}, {-0.5f, 0.5f, 0}},
    {4, 3, {1, 2, 4}, {0.8f, 0.5f, 1}, {-0.6f, 0.6f, 0}},
    {8, 3, {3, 6, 8}, {1, 0.8f, 0.9f}, {-0.7f, 0.7f, 0}},
    {8, 5, {2, 3, 5, 6, 8}, {0.6f, 0.9f, 0.5f, 0.8f, 1}, {-0.8f, 0.4f, -0.3f, 0.8f, 0}},
    {6, 6, {1, 2, 3, 4, 5, 6}, {0.25f, 0.4f, 0.55f, 0.7f, 0.85f, 1}, {-1, 1, -0.6f, 0.6f, -0.2f, 0.2f}},
    {7, 4, {1, 3, 4, 7}, {0.5f, 0.9f, 0.4f, 1}, {0.9f, -0.9f, 0.3f, -0.2f}},
};
static const char *const PATN[8] = {"ECHO", "PING-PONG", "TRIPLE", "GALLOP", "CLAVE", "EUCLID 5/8", "CASCADE", "SCATTER"};

static float rhythm_step(float v, float bpm)
{
    return 60.f / clampf(bpm, 40.f, 240.f) * DIV[pstep(v, 12)];
}

static void rhythm(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: DUCK, WOBBLE RATE, ACCENT (how much the pattern's loud taps stand out) */
    float wrate = expmap(p[9], 0.1f, 6.f), accent = p[10] * 2.f;
    const Pattern *pat = &PAT[pstep(raw[2], 8)];
    float step = rhythm_step(raw[0], pt->bpm) * TFX_SR;
    float maxstep = (float)(DMEM - TFX_SR / 4) / (float)pat->len;
    if (step > maxstep) step = maxstep;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 1.0f, tone = p[3] * 2.f - 1.f, swing = p[4] * 0.5f;
    float spread = p[5], wob = p[6] * 0.003f * TFX_SR, diff = p[7];
    float lpk = tone < 0 ? onepole_coef(expmap(1.f + tone, 500.f, 18000.f)) : 1.f;
    float hpk = tone > 0 ? onepole_coef(expmap(tone, 20.f, 2500.f)) : 0.f;
    if (freeze) lpk = 1.f, hpk = 0.f;
    float tg = 0.f;
    float pg[8];
    for (int t = 0; t < pat->n; t++) pg[t] = powf(pat->gain[t], accent), tg += pg[t];
    float norm = 1.2f / sqrtf(tg);
    if (d->time_s <= 0.f) d->time_s = step;
    if (!d->gon) d->gsw = swing, d->gon = 1;
    for (int i = 0; i < n; i++) {
        d->time_s += (step - d->time_s) * 0.0005f;
        float sw = tap_glide(&d->gsw, swing);
        d->wph += wrate / TFX_SR;
        d->wph -= floorf(d->wph);
        float wo = wob * (0.6f * lfo_sin(d->wph) + 0.4f * wander_tick(&d->ww, &d->rng, 1.3f));
        float st = d->time_s;
        float x[2] = {l[i], r[i]};
        float out[2] = {0, 0};
        for (int c = 0; c < 2; c++) {
            for (int t = 0; t < pat->n; t++) {
                float s = pat->step[t];
                float dt = s * st + (((int)s & 1) ? sw * st : 0.f) + wo;
                float pan = pat->pan[t] * spread;
                float g = pg[t] * sqrtf(0.5f * (1.f + (c ? pan : -pan)));
                out[c] += g * dly_lin(&d->b[c], dt);
            }
        }
        for (int c = 0; c < 2; c++) {
            /* the whole pattern repeats: the loop is pattern-long */
            float y = dly_lin(&d->b[c], pat->len * st + wo);
            d->lp[c] += lpk * (y - d->lp[c]);
            d->hp[c] += hpk * (d->lp[c] - d->hp[c]);
            float v = d->lp[c] - d->hp[c];
            if (diff > 0.01f) {
                float a = ap_run(&d->diff[c][0], v, 0.65f * diff);
                a = ap_run(&d->diff[c][1], a, 0.6f * diff);
                v = lerpf(v, a, diff);
            }
            dly_push(&d->b[c], (freeze ? 0.f : x[c]) + fb * (freeze ? v : softclip(v)));
        }
        l[i] = out[0] * norm;
        r[i] = out[1] * norm;
    }
}

/* ---------------------------------------------------------------- REVERSO */
static void reverso(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, DUCK, SPLICE (the reverse heads' crossfade: smooth .. short), DRIFT RATE */
    float splice = p[10], drate = expmap(p[11], 0.05f, 3.f);
    float T = synced(raw[8], pt->bpm, expmap(p[0], 0.06f, 2.5f)) * TFX_SR;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 0.95f, dir = p[2], smear = p[3] * 0.72f, drift = p[4] * 0.015f;
    float lpk = freeze ? 1.f : onepole_coef(expmap(p[5], 900.f, 18000.f)), spread = p[6], swell = p[7];
    if (d->time_s <= 0.f) d->time_s = T;
    float stime = 0.002f + swell * swell * 3.f; /* the swell's rise, s */
    if (!d->gon) d->gsp = spread, d->gon = 1;
    for (int i = 0; i < n; i++) {
        d->time_s += (T - d->time_s) * 0.0003f;
        float sp = tap_glide(&d->gsp, spread);
        float dr = 1.f + drift * wander_tick(&d->rw, &d->rng, drate);
        /* auto-swell: duck at each new phrase, then rise */
        float lev = fabsf(l[i]) + fabsf(r[i]);
        d->env_f += (lev - d->env_f) * (lev > d->env_f ? 0.02f : 0.002f);
        d->env_s += (lev - d->env_s) * 0.00007f;
        if (swell > 0.02f) {
            if (d->env_f > 2.5f * d->env_s + 0.003f && d->swell > 0.5f) d->swell = 0.499f; /* start a duck */
            if (d->swell < 0.5f && d->swell > 0.01f && d->env_f > 0.f) d->swell *= 0.995f;
            else d->swell = clampf(d->swell + 1.f / (stime * TFX_SR), 0.f, 1.f);
            if (d->swell <= 0.01f) d->swell = 0.5f; /* ducked: rise from here */
        } else
            d->swell = 1.f;
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float Tc = d->time_s * (c ? 1.f + 0.5f * sp : 1.f) * dr;
            float fwd = dly_lin(&d->b[c], Tc);
            /* two reverse heads half a segment apart: each reads backwards through the last Tc */
            float rev = 0.f;
            d->rc[c] += 1.f;
            if (d->rc[c] >= Tc) d->rc[c] -= Tc;
            for (int h = 0; h < 2; h++) {
                float cc = d->rc[c] + (h ? 0.5f * Tc : 0.f);
                if (cc >= Tc) cc -= Tc;
                float ph = cc / Tc;
                float w = 0.5f - 0.5f * lfo_sin(ph + 0.25f);
                if (splice > 0.f) w = lerpf(w, fminf(w * 8.f, 1.f) * 0.5f + 0.5f * w, splice);
                rev += w * dly_lin(&d->b[c], 2.f * cc + 2.f);
            }
            float o = lerpf(fwd, rev, dir);
            /* the feedback: smeared through all-passes, darkened */
            float v = o;
            if (smear > 0.01f)
                for (int k = 0; k < 3; k++) v = ap_run(&d->rdiff[c][k], v, smear);
            d->lp[c] += lpk * (v - d->lp[c]);
            dly_push(&d->b[c], (freeze ? 0.f : x[c]) + fb * (freeze ? d->lp[c] : softclip(d->lp[c])));
            float y = lerpf(o, v, smear * 0.6f) * (0.5f + 0.5f * (swell > 0.02f ? (d->swell > 0.5f ? d->swell : 0.f) * 2.f - 1.f : 1.f));
            if (c) r[i] = y; else l[i] = y;
        }
    }
}

/* ---------------------------------------------------------------- TAPE */
static const int HEADS[7] = {1, 2, 4, 3, 6, 5, 7}; /* bits: head 1, 2, 3 */
static const char *const HEADN[7] = {"1", "2", "3", "1+2", "2+3", "1+3", "1+2+3"};

static void tape(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC (the longest head), DUCK, WOW RATE, FLUTTER RATE, SPACING (the heads evenly spaced,
     * bunched toward the long head, or toward the short) */
    float wr = expmap(p[10], 0.1f, 3.f), fr = expmap(p[11], 2.f, 20.f), sexp = expmap(p[12], 0.5f, 2.f);
    float T = synced(raw[8], pt->bpm, expmap(p[0], 0.04f, 1.2f)) * TFX_SR;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 1.15f;
    int heads = HEADS[pstep(raw[2], 7)];
    float wow = p[3] * 0.006f, flut = p[4] * 0.0015f, age = p[5];
    float drive = 1.f + p[6] * 4.f, spread = p[7];
    float lpk = onepole_coef(expmap(1.f - age, 1800.f, 12000.f)), hpk = onepole_coef(60.f + age * 200.f);
    int nh = (heads & 1) + ((heads >> 1) & 1) + ((heads >> 2) & 1);
    float hn = 1.f / sqrtf((float)nh);
    int last = heads & 4 ? 2 : heads & 2 ? 1 : 0;
    static const float HP[3] = {-1.f, 1.f, 0.f};
    if (d->time_s <= 0.f) d->time_s = T;
    for (int i = 0; i < n; i++) {
        d->time_s += (T - d->time_s) * 0.00006f; /* the motor: slow, so time changes glide in pitch */
        d->tph_w += wr / TFX_SR; d->tph_w -= floorf(d->tph_w);
        d->tph_f += fr / TFX_SR; d->tph_f -= floorf(d->tph_f);
        float sp = 1.f + wow * (0.7f * lfo_sin(d->tph_w) + 0.3f * wander_tick(&d->tw, &d->rng, 0.9f)) +
                   flut * lfo_sin(d->tph_f);
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float out = 0.f, loop = 0.f;
            for (int h = 0; h < 3; h++) {
                if (!(heads & (1 << h))) continue;
                float y = dly_herm(&d->b[c], d->time_s * sp * powf((float)(h + 1) / 3.f, sexp) + (c ? 7.f : 0.f));
                float pan = HP[h] * spread;
                out += y * hn * sqrtf(1.f + (c ? pan : -pan)) * 0.75f;
                if (h == last) loop = y;
            }
            /* the tape: band-limited, saturating, a little hiss as it ages */
            d->lp[c] += lpk * (loop - d->lp[c]);
            d->hp[c] += hpk * (d->lp[c] - d->hp[c]);
            float v = d->lp[c] - d->hp[c];
            float in = (freeze ? 0.f : x[c]) * drive + fb * v + age * 0.0004f * rng_bi(&d->rng);
            float s = softclip(in * 0.8f) * 1.25f / (0.6f + 0.4f * drive);
            dly_push(&d->b[c], s);
            if (c) r[i] = out; else l[i] = out;
        }
    }
}

/* ---------------------------------------------------------------- SHIMMER */
static const float SHS[6] = {12, 7, 5, -12, 19, 24};
static const char *const SHSN[6] = {"+OCTAVE", "+FIFTH", "+FOURTH", "-OCTAVE", "+OCT+5TH", "+2 OCT"};

static void shimmer(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, DUCK, WINDOW (the pitch shifter's: short is grainy, long is smooth), DRIFT RATE */
    float drate = expmap(p[11], 0.05f, 3.f);
    float T = synced(raw[8], pt->bpm, expmap(p[0], 0.08f, 2.f)) * TFX_SR;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 0.92f, ratio = exp2f(SHS[pstep(raw[2], 6)] / 12.f), smix = p[3];
    float lpk = onepole_coef(expmap(p[4], 1500.f, 18000.f)), diff = p[5] * 0.7f, spread = p[6], drift = p[7] * 0.01f;
    if (d->time_s <= 0.f) d->time_s = T;
    const float W = expmap(p[10], 0.02f, 0.12f) * TFX_SR;
    if (!d->gon) d->gsp = spread, d->gon = 1;
    for (int i = 0; i < n; i++) {
        d->time_s += (T - d->time_s) * 0.0003f;
        float sp = tap_glide(&d->gsp, spread);
        float dr = 1.f + drift * wander_tick(&d->ww, &d->rng, drate);
        d->shph += (1.f - ratio) / W;
        d->shph -= floorf(d->shph);
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float y = dly_lin(&d->b[c], d->time_s * (c ? 1.f + 0.33f * sp : 1.f) * dr);
            dly_push(&d->psh[c], y);
            float ps = 0.f;
            for (int h = 0; h < 2; h++) {
                float ph = d->shph + 0.5f * h;
                ph -= floorf(ph);
                ps += (0.5f - 0.5f * lfo_sin(ph + 0.25f)) * dly_lin(&d->psh[c], 2.f + ph * W);
            }
            float v = lerpf(y, ps, smix);
            if (diff > 0.01f) v = lerpf(v, ap_run(&d->diff[c][1], ap_run(&d->diff[c][0], v, diff), diff), diff);
            d->lp[c] += lpk * (v - d->lp[c]);
            dly_push(&d->b[c], (freeze ? 0.f : x[c]) + fb * softclip(d->lp[c]));
            if (c) r[i] = v; else l[i] = v;
        }
    }
}

/* ---------------------------------------------------------------- CLOUD */
static const float CLR[4] = {1.f, 1.37f, 0.71f, 1.83f};

static void cloud(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, DUCK RELEASE (how slowly the haze comes back), WANDER RATE */
    float drel = expmap(p[9], 0.25f, 4.f), wsc = expmap(p[10], 0.25f, 4.f);
    float T = synced(raw[8], pt->bpm, expmap(p[0], 0.1f, 1.f)) * TFX_SR;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 0.9f, diff = p[2] * 0.75f, mod = p[3] * 0.012f * TFX_SR;
    float lpk = freeze ? 1.f : onepole_coef(expmap(p[4], 1200.f, 16000.f)), spread = p[5], bloom = p[6], duck = p[7];
    if (d->time_s <= 0.f) d->time_s = T;
    for (int i = 0; i < n; i++) {
        d->time_s += (T - d->time_s) * 0.0003f;
        float lev = fabsf(l[i]) + fabsf(r[i]);
        d->env_f += (lev - d->env_f) * (lev > d->env_f ? 0.01f : 0.0004f / drel);
        float dk = 1.f / (1.f + duck * 12.f * d->env_f);
        d->cduck += (dk - d->cduck) * (dk < d->cduck ? 0.002f : 0.002f / drel);
        float in[2];
        for (int c = 0; c < 2; c++) {
            float x = freeze ? 0.f : (c ? r[i] : l[i]);
            x = ap_run(&d->cin[c][0], x, diff);
            in[c] = ap_run(&d->cin[c][1], x, diff * 0.9f);
        }
        float y[4];
        for (int k = 0; k < 4; k++) {
            float t = d->time_s * lerpf(1.f, CLR[k], spread) + mod * (1.f + wander_tick(&d->cw[k], &d->rng, (0.2f + 0.13f * k) * wsc));
            float z = dly_lin(&d->cl[k], t + 8.f);
            d->clp[k] += lpk * (z - d->clp[k]);
            y[k] = d->clp[k];
        }
        /* a 4x4 Hadamard: each echo feeds all the others, so they smear into a cloud as BLOOM rises */
        float h0 = y[0] + y[1] + y[2] + y[3], h1 = y[0] - y[1] + y[2] - y[3];
        float h2 = y[0] + y[1] - y[2] - y[3], h3 = y[0] - y[1] - y[2] + y[3];
        float hm[4] = {h0 * 0.5f, h1 * 0.5f, h2 * 0.5f, h3 * 0.5f};
        for (int k = 0; k < 4; k++) {
            float back = lerpf(y[k], hm[k], bloom);
            dly_push(&d->cl[k], in[k & 1] + fb * softclip(back));
        }
        l[i] = (y[0] + 0.6f * y[2]) * d->cduck * 0.8f;
        r[i] = (y[1] + 0.6f * y[3]) * d->cduck * 0.8f;
    }
}

/* ---------------------------------------------------------------- STUTTER */
static void stutter(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: FADE (each repeat's edges), LOOK BACK (catch the slice 1-4 slices ago) */
    float fadeL = expmap(p[8], 16.f, 2000.f);
    int back = 1 + pstep(raw[9], 4);
    float slice = rhythm_step(raw[0], pt->bpm) * TFX_SR;
    float chance = p[1];
    int reps = 2 + (int)(p[2] * 14.f);
    float pitch = (p[3] * 2.f - 1.f) * 12.f, rev = p[4], decay = 0.55f + 0.45f * p[5], gate = 0.25f + 0.75f * p[6], spread = p[7];
    const float N = (float)(d->b[0].mask + 1);
    const float fade = fadeL;
    for (int i = 0; i < n; i++) {
        float x[2] = {l[i], r[i]};
        if (!pt->freeze_dly) {
            dly_push(&d->b[0], x[0]);
            dly_push(&d->b[1], x[1]);
        }
        /* the grid: at each slice boundary, maybe catch the slice just gone */
        d->st_clock += 1.f;
        if (d->st_clock >= slice) {
            d->st_clock -= slice;
            if (d->st_on) {
                if (--d->st_left <= 0) d->st_on = 0;
                else {
                    d->st_k++;
                    d->st_gain *= decay;
                    d->st_rate = exp2f(pitch * d->st_k / 12.f / (float)reps * 2.f);
                    d->st_pos = 0.f;
                    d->st_pan = -d->st_pan;
                }
            } else if (rng_uni(&d->rng) < chance || pt->freeze_dly) {
                d->st_on = 1;
                d->st_left = reps;
                d->st_k = 0;
                d->st_gain = 1.f;
                d->st_rate = 1.f;
                d->st_pos = 0.f;
                d->st_len = slice;
                d->st_rev = rng_uni(&d->rng) < rev;
                d->st_pan = spread * (rng_uni(&d->rng) < 0.5f ? -1.f : 1.f);
                d->st_start = (float)d->b[0].w - slice * (float)back;
                if (d->st_start < 0.f) d->st_start += N;
            }
        }
        if (d->st_on) {
            float len = d->st_len * gate;
            float ph = d->st_pos;
            float w = 0.f;
            if (ph < len) {
                float fd = fminf(fade, 0.5f * len);
                w = ph < fd ? ph / fd : 1.f;
                if (len - ph < fd) w = fminf(w, (len - ph) / fd);
            }
            float at = d->st_rev ? d->st_start + d->st_len - 1.f - ph : d->st_start + ph;
            while (at >= N) at -= N;
            while (at < 0.f) at += N;
            float g = w * d->st_gain;
            float a = dly_at(&d->b[0], at) * g, b = dly_at(&d->b[1], at) * g;
            l[i] = a * sqrtf(1.f - d->st_pan * 0.5f) ;
            r[i] = b * sqrtf(1.f + d->st_pan * 0.5f);
            d->st_pos += d->st_rate;
            /* between repeats the input does not come through: the stutter owns the output */
        }
        /* idle: the input passes, so at full wet it is a glitch insert */
    }
}

/* ---------------------------------------------------------------- DUB */
static void dub(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, DUCK, WOBBLE RATE */
    float wrate = expmap(p[10], 0.1f, 3.f);
    float T = synced(raw[8], pt->bpm, expmap(p[0], 0.05f, 1.5f)) * TFX_SR;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 1.2f, g = svf_g(expmap(p[2], 100.f, 8000.f)), k = 1.f / expmap(p[3], 0.6f, 10.f);
    float type = p[4], drive = 1.f + p[5] * 4.f, spread = p[6], wob = p[7] * 0.004f * TFX_SR;
    if (d->time_s <= 0.f) d->time_s = T;
    if (!d->gon) d->gsp = spread, d->gon = 1;
    for (int i = 0; i < n; i++) {
        d->time_s += (T - d->time_s) * 0.0001f;
        float sp = tap_glide(&d->gsp, spread);
        d->wph += wrate / TFX_SR; d->wph -= floorf(d->wph);
        float wo = wob * (0.5f + 0.5f * lfo_sin(d->wph));
        float y[2], x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) y[c] = dly_lin(&d->b[c], d->time_s * (c ? 1.f + 0.5f * sp : 1.f) + wo + 4.f);
        for (int c = 0; c < 2; c++) {
            /* ping-pong as SPREAD rises: each side's repeats come back on the other */
            float back = lerpf(y[c], y[1 - c], spread);
            SvfOut f = svf_run(&d->dsv[c], back, g, k);
            float bp = f.bp * k * 1.2f;
            float v = type < 0.5f ? lerpf(f.lp, bp, type * 2.f) : lerpf(bp, f.hp, (type - 0.5f) * 2.f);
            /* resonance lifts the loop at the cutoff: only high RESO and FEEDBACK together run away */
            v *= 0.35f + 0.65f * k;
            v = softclip(v * drive) / drive;
            dly_push(&d->b[c], (freeze ? 0.f : x[c]) + fb * v);
            d->dfb[c] = v;
        }
        l[i] = y[0];
        r[i] = y[1];
    }
}

/* ---------------------------------------------------------------- PRISM */
static const float PBAND[4] = {160.f, 650.f, 2600.f, 9000.f};

static void prism(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* TIME, FEEDBACK, SPREAD (bipolar: + the highs come back later, - the lows), TILT (the bands'
     * levels: dark .. bright), PAN (the bands across the stereo field), DIFFUSE, DRIFT (each band's
     * time wanders on its own), WIDTH (how wide each band is: narrow bands ring)
     * DEEP: SYNC, DUCK */
    float T = synced(raw[8], pt->bpm, expmap(p[0], 0.05f, 2.f)) * TFX_SR;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 0.95f, spread = (p[2] * 2.f - 1.f) * 1.6f, tilt = p[3] * 2.f - 1.f;
    float pan = p[4], diff = p[5] * 0.7f, drift = p[6] * 0.03f, k = 1.f / expmap(p[7], 0.7f, 6.f);
    float bg[4], gl[4], gr[4], g[4], tot = 0.f;
    for (int b = 0; b < 4; b++) {
        float pos = (b - 1.5f) / 1.5f;
        bg[b] = exp2f(tilt * pos * 1.2f);
        float pp = pan * (b & 1 ? 1.f : -1.f) * (0.5f + 0.5f * fabsf(pos));
        gl[b] = sqrtf(0.5f * (1.f - pp)), gr[b] = sqrtf(0.5f * (1.f + pp));
        g[b] = svf_g(PBAND[b]);
        tot += bg[b];
    }
    float norm = 4.f / tot * (0.9f + 0.6f * (1.f - k)) ;
    float maxT = (float)(DMEM - TFX_SR) / 2.f;
    for (int i = 0; i < n; i++) {
        float x[2] = {l[i], r[i]};
        float ol = 0.f, orr = 0.f, back[2] = {0.f, 0.f};
        for (int b = 0; b < 4; b++) {
            float want = T * exp2f(spread * (b - 1.5f) / 1.5f) * (1.f + drift * wander_tick(&d->pw[b], &d->rng, 0.25f + 0.11f * b));
            if (want > maxT) want = maxT;
            if (d->ptime[b] <= 0.f) d->ptime[b] = want;
            d->ptime[b] += (want - d->ptime[b]) * 0.0002f;
            for (int c = 0; c < 2; c++) {
                float y = dly_lin(&d->b[c], d->ptime[b] + 4.f);
                float v = k * svf_run(&d->psv[c][b], y, g[b], k).bp * norm * bg[b];
                back[c] += v;
                ol += v * gl[b] * (c ? 0.3f : 1.f);
                orr += v * gr[b] * (c ? 1.f : 0.3f);
            }
        }
        for (int c = 0; c < 2; c++) {
            float v = back[c] * 0.5f;
            if (diff > 0.01f) v = lerpf(v, ap_run(&d->diff[c][1], ap_run(&d->diff[c][0], v, diff), diff), diff);
            dly_push(&d->b[c], (freeze ? 0.f : x[c]) + fb * (freeze ? v : softclip(v)));
        }
        l[i] = ol * 0.7f;
        r[i] = orr * 0.7f;
    }
}

/* ---------------------------------------------------------------- ERODE */
static void erode(Delay *d, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* TIME, FEEDBACK, ERODE (how much slower each pass is sampled), GRIT (how many bits each pass
     * keeps), TONE, DROPOUTS, SPREAD, WOBBLE
     * DEEP: SYNC, DUCK, HISS (noise each pass adds: old memory) */
    float T = synced(raw[8], pt->bpm, expmap(p[0], 0.05f, 2.f)) * TFX_SR;
    int freeze = pt->freeze_dly;
    float fb = freeze ? 1.f : p[1] * 1.02f;
    float rate = expmap(1.f - p[2], 1500.f, 48000.f) / TFX_SR;
    float bits = expmap(1.f - p[3], 3.f, 16.f), q = exp2f(1.f - bits);
    float lpk = onepole_coef(expmap(p[4], 800.f, 18000.f)), drop = p[5], spread = p[6], wob = p[7] * 0.004f * TFX_SR;
    float hiss = p[10] * p[10] * 0.01f;
    if (d->time_s <= 0.f) d->time_s = T;
    if (!d->gon) d->gsp = spread, d->gon = 1;
    for (int i = 0; i < n; i++) {
        d->time_s += (T - d->time_s) * 0.0002f;
        float sp = tap_glide(&d->gsp, spread);
        d->wph += 0.37f / TFX_SR; d->wph -= floorf(d->wph);
        float wo = wob * (0.5f + 0.5f * (0.6f * lfo_sin(d->wph) + 0.4f * wander_tick(&d->ew, &d->rng, 1.7f)));
        /* dropouts: now and then a stretch of the memory reads back as nothing */
        if (rng_uni(&d->rng) < drop * drop * 0.0004f) d->etgt = 0.f;
        else if (d->etgt < 0.5f && rng_uni(&d->rng) < 0.0008f) d->etgt = 1.f;
        d->egate += (d->etgt - d->egate) * 0.004f;
        /* the clock: the pass is sampled (and held) slower as ERODE rises */
        d->eph += rate;
        int tick = d->eph >= 1.f;
        if (tick) d->eph -= floorf(d->eph);
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float y = dly_lin(&d->b[c], d->time_s * (c ? 1.f + 0.5f * sp : 1.f) + wo + 4.f);
            if (tick) d->eh[c] = q * floorf(y / q + 0.5f);
            float v = d->eh[c] * d->egate;
            d->lp[c] += lpk * (v - d->lp[c]);
            v = d->lp[c];
            d->ehiss[c] = 0.9f * d->ehiss[c] + 0.1f * rng_bi(&d->rng);
            dly_push(&d->b[c], (freeze ? 0.f : x[c]) + fb * softclip(v + hiss * d->ehiss[c]));
            if (c) r[i] = y; else l[i] = y;
        }
    }
}

/* which deep parameter is DUCK (-1: none) */
static const int DUCK_P[DL_N] = {9, 8, 9, 9, 9, -1, -1, 9, 9, 9};

void delay_process(Delay *d, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    float lev[MOD_BLOCK];
    int dp = DUCK_P[algo];
    float duck = dp >= 0 ? p[dp] : 0.f;
    if (duck > 0.001f)
        for (int i = 0; i < n; i++) lev[i] = 0.5f * (fabsf(l[i]) + fabsf(r[i]));
    switch (algo) {
    case DL_GRAIN: granular(d, p, raw, pt, l, r, n); break;
    case DL_RHYTHM: rhythm(d, p, raw, pt, l, r, n); break;
    case DL_REVERSO: reverso(d, p, raw, pt, l, r, n); break;
    case DL_TAPE: tape(d, p, raw, pt, l, r, n); break;
    case DL_SHIMMER: shimmer(d, p, raw, pt, l, r, n); break;
    case DL_CLOUD: cloud(d, p, raw, pt, l, r, n); break;
    case DL_STUTTER: stutter(d, p, raw, pt, l, r, n); break;
    case DL_DUB: dub(d, p, raw, pt, l, r, n); break;
    case DL_PRISM: prism(d, p, raw, pt, l, r, n); break;
    default: erode(d, p, raw, pt, l, r, n); break;
    }
    /* DUCK: the repeats step back while you play, and swell up in the gaps */
    if (duck > 0.001f) {
        for (int i = 0; i < n; i++) {
            d->dk_env += (lev[i] - d->dk_env) * (lev[i] > d->dk_env ? 0.01f : 0.0003f);
            float g = 1.f / (1.f + duck * 16.f * d->dk_env);
            d->dk_g += (g - d->dk_g) * (g < d->dk_g ? 0.003f : 0.0004f);
            l[i] *= d->dk_g;
            r[i] *= d->dk_g;
        }
    } else
        d->dk_g = 1.f, d->dk_env = 0.f;
}

/* ---------------------------------------------------------------- the UI side */
static void secs(char *o, int n, float s) { snprintf(o, n, s < 1.f ? "%.0f ms" : "%.2f s", s < 1.f ? s * 1000.f : s); }

static void fmt(int algo, int p, float v, const Patch *pt, char *o, int n)
{
    if (p >= NBOARD) {
        int dd = p - NBOARD;
        float bpm = pt ? pt->bpm : 120.f;
        if (dd == 0 && algo != DL_RHYTHM && algo != DL_STUTTER) {
            int s = pstep(v, 13);
            if (!s) snprintf(o, n, "FREE");
            else snprintf(o, n, "%s %.0fms", DIVN[s - 1], 60.f / clampf(bpm, 40.f, 240.f) * DIV[s - 1] * 1000.f);
            return;
        }
        if (dd == (DUCK_P[algo] - NBOARD)) { snprintf(o, n, v < 0.01f ? "OFF" : "%.0f %%", v * 100.f); return; }
        switch (algo) {
        case DL_GRAIN:
            if (dd == 2) { snprintf(o, n, v < 0.03f ? "BELL" : v > 0.97f ? "FLAT" : "%.0f %% FLAT", v * 100.f); return; }
            if (dd == 3) { snprintf(o, n, "%.0f Hz", expmap(v, 500.f, 18000.f)); return; }
            if (dd == 4) { snprintf(o, n, "+-%.2f st", v * 0.5f); return; }
            break;
        case DL_RHYTHM:
            if (dd == 1) { snprintf(o, n, "%.2f Hz", expmap(v, 0.1f, 6.f)); return; }
            if (dd == 2) { snprintf(o, n, v < 0.02f ? "EVEN" : "x%.1f", v * 2.f); return; }
            break;
        case DL_REVERSO:
            if (dd == 2) { snprintf(o, n, v < 0.03f ? "SMOOTH" : "%.0f %% SHORT", v * 100.f); return; }
            if (dd == 3) { snprintf(o, n, "%.2f Hz", expmap(v, 0.05f, 3.f)); return; }
            break;
        case DL_TAPE:
            if (dd == 2) { snprintf(o, n, "%.2f Hz", expmap(v, 0.1f, 3.f)); return; }
            if (dd == 3) { snprintf(o, n, "%.1f Hz", expmap(v, 2.f, 20.f)); return; }
            if (dd == 4) { float e = expmap(v, 0.5f, 2.f); snprintf(o, n, fabsf(e - 1.f) < 0.05f ? "EVEN" : e > 1.f ? "BUNCHED LONG" : "BUNCHED SHORT"); return; }
            break;
        case DL_SHIMMER:
            if (dd == 2) { snprintf(o, n, "%.0f ms", expmap(v, 0.02f, 0.12f) * 1000.f); return; }
            if (dd == 3) { snprintf(o, n, "%.2f Hz", expmap(v, 0.05f, 3.f)); return; }
            break;
        case DL_CLOUD:
            if (dd == 1 || dd == 2) { snprintf(o, n, "x%.2f", expmap(v, 0.25f, 4.f)); return; }
            break;
        case DL_STUTTER:
            if (dd == 0) { snprintf(o, n, "%.0f ms", expmap(v, 16.f, 2000.f) / 48.f); return; }
            if (dd == 1) { snprintf(o, n, "%d SLICE%s", 1 + pstep(v, 4), pstep(v, 4) ? "S" : ""); return; }
            break;
        case DL_DUB:
            if (dd == 2) { snprintf(o, n, "%.2f Hz", expmap(v, 0.1f, 3.f)); return; }
            break;
        }
        snprintf(o, n, "%.0f %%", v * 100.f);
        return;
    }
    switch (algo) {
    case DL_GRAIN:
        if (p == 0) { secs(o, n, expmap(v, 0.01f, 3.f)); return; }
        if (p == 2) { snprintf(o, n, "%+.0f st", SEMI[pstep(v, 10)]); return; }
        if (p == 3) { snprintf(o, n, "%.1f /s", expmap(v, 1.f, 60.f)); return; }
        if (p == 4) { secs(o, n, expmap(v, 0.015f, 0.6f)); return; }
        break;
    case DL_RHYTHM:
        if (p == 0) { snprintf(o, n, "%s %.0fms", DIVN[pstep(v, 12)], rhythm_step(v, pt ? pt->bpm : 120.f) * 1000.f); return; }
        if (p == 1) { snprintf(o, n, "%.0f %%", v * 100.f); return; }
        if (p == 2) { snprintf(o, n, "%s", PATN[pstep(v, 8)]); return; }
        if (p == 3) {
            float t = v * 2.f - 1.f;
            if (fabsf(t) < 0.03f) snprintf(o, n, "FLAT");
            else if (t < 0) snprintf(o, n, "LP %.0f Hz", expmap(1.f + t, 500.f, 18000.f));
            else snprintf(o, n, "HP %.0f Hz", expmap(t, 20.f, 2500.f));
            return;
        }
        break;
    case DL_REVERSO:
        if (p == 0) { secs(o, n, expmap(v, 0.06f, 2.5f)); return; }
        if (p == 2) { snprintf(o, n, v < 0.03f ? "FORWARD" : v > 0.97f ? "REVERSE" : "%.0f%% REV", v * 100.f); return; }
        if (p == 5) { snprintf(o, n, "%.0f Hz", expmap(v, 900.f, 18000.f)); return; }
        if (p == 7) { if (v <= 0.02f) snprintf(o, n, "OFF"); else secs(o, n, 0.002f + v * v * 3.f); return; }
        break;
    case DL_TAPE:
        if (p == 0) { secs(o, n, expmap(v, 0.04f, 1.2f)); return; }
        if (p == 1) { snprintf(o, n, "%.0f %%", v * 115.f); return; }
        if (p == 2) { snprintf(o, n, "%s", HEADN[pstep(v, 7)]); return; }
        break;
    case DL_SHIMMER:
        if (p == 0) { secs(o, n, expmap(v, 0.08f, 2.f)); return; }
        if (p == 2) { snprintf(o, n, "%s", SHSN[pstep(v, 6)]); return; }
        if (p == 4) { snprintf(o, n, "%.0f Hz", expmap(v, 1500.f, 18000.f)); return; }
        break;
    case DL_CLOUD:
        if (p == 0) { secs(o, n, expmap(v, 0.1f, 1.f)); return; }
        if (p == 4) { snprintf(o, n, "%.0f Hz", expmap(v, 1200.f, 16000.f)); return; }
        break;
    case DL_STUTTER:
        if (p == 0) { snprintf(o, n, "%s %.0fms", DIVN[pstep(v, 12)], rhythm_step(v, pt ? pt->bpm : 120.f) * 1000.f); return; }
        if (p == 2) { snprintf(o, n, "%d", 2 + (int)(v * 14.f)); return; }
        if (p == 3) { snprintf(o, n, "%+.1f st", (v * 2.f - 1.f) * 12.f); return; }
        break;
    case DL_PRISM:
        if (p == 0) { secs(o, n, expmap(v, 0.05f, 2.f)); return; }
        if (p == 2) { float sp = (v * 2.f - 1.f); snprintf(o, n, fabsf(sp) < 0.03f ? "TOGETHER" : sp > 0 ? "HIGHS LATE %.0f%%" : "LOWS LATE %.0f%%", fabsf(sp) * 100.f); return; }
        if (p == 3) { snprintf(o, n, "%+.0f", (v * 2.f - 1.f) * 100.f); return; }
        if (p == 7) { snprintf(o, n, "Q %.1f", expmap(v, 0.7f, 6.f)); return; }
        break;
    case DL_ERODE:
        if (p == 0) { secs(o, n, expmap(v, 0.05f, 2.f)); return; }
        if (p == 1) { snprintf(o, n, "%.0f %%", v * 102.f); return; }
        if (p == 2) { snprintf(o, n, "%.1f kHz", expmap(1.f - v, 1500.f, 48000.f) / 1000.f); return; }
        if (p == 3) { snprintf(o, n, "%.1f bit", expmap(1.f - v, 3.f, 16.f)); return; }
        if (p == 4) { snprintf(o, n, "%.0f Hz", expmap(v, 800.f, 18000.f)); return; }
        break;
    case DL_DUB:
        if (p == 0) { secs(o, n, expmap(v, 0.05f, 1.5f)); return; }
        if (p == 1) { snprintf(o, n, "%.0f %%", v * 120.f); return; }
        if (p == 2) { snprintf(o, n, "%.0f Hz", expmap(v, 100.f, 8000.f)); return; }
        if (p == 4) { snprintf(o, n, v < 0.15f ? "LOW-PASS" : v > 0.85f ? "HIGH-PASS" : (v > 0.4f && v < 0.6f) ? "BAND-PASS" : "%.0f %%", v * 100.f); return; }
        break;
    }
    snprintf(o, n, "%.0f %%", v * 100.f);
}

const ModInfo delay_info = {
    "DELAY",
    DL_N,
    {"GRANULAR", "RHYTHM", "REVERSO", "TAPE", "SHIMMER", "CLOUD", "STUTTER", "DUB", "PRISM", "ERODE"},
    {"a grain cloud from the delay: pitch, spray, reverse",
     "tempo-synced tap patterns, swing, diffusing repeats",
     "forward / reverse heads: repeats flip, smear and swell",
     "three-head tape echo: wow, flutter, age, runaway",
     "every repeat climbs an interval, diffused",
     "four wandering echoes that bloom into haze, ducking",
     "beat-repeat: catches slices, pitches and flips them",
     "dub echo through a morphing resonant filter, runaway",
     "four bands, four times: every echo smears into its colours",
     "a failing memory: each repeat crumbles into dust"},
    {
        {"TIME", "FEEDBACK", "PITCH", "DENSITY", "SIZE", "SPRAY", "REVERSE", "SPARKLE", "SYNC", "DUCK", "ENVELOPE", "FB TONE", "DETUNE"},
        {"DIVISION", "FEEDBACK", "PATTERN", "TONE", "SWING", "SPREAD", "WOBBLE", "DIFFUSE", "DUCK", "WOBBLE RATE", "ACCENT"},
        {"TIME", "FEEDBACK", "DIRECTION", "SMEAR", "DRIFT", "TONE", "SPREAD", "SWELL", "SYNC", "DUCK", "SPLICE", "DRIFT RATE"},
        {"TIME", "FEEDBACK", "HEADS", "WOW", "FLUTTER", "AGE", "DRIVE", "SPREAD", "SYNC", "DUCK", "WOW RATE", "FLUTTER RATE", "SPACING"},
        {"TIME", "FEEDBACK", "INTERVAL", "SHIFT", "TONE", "DIFFUSE", "SPREAD", "DRIFT", "SYNC", "DUCK", "WINDOW", "DRIFT RATE"},
        {"TIME", "FEEDBACK", "DIFFUSION", "MOD", "TONE", "SPREAD", "BLOOM", "DUCK", "SYNC", "DUCK RELEASE", "WANDER RATE"},
        {"DIVISION", "CHANCE", "REPEATS", "PITCH", "REVERSE", "DECAY", "GATE", "SPREAD", "FADE", "LOOK BACK"},
        {"TIME", "FEEDBACK", "FILTER", "RESO", "TYPE", "DRIVE", "SPREAD", "WOBBLE", "SYNC", "DUCK", "WOBBLE RATE"},
        {"TIME", "FEEDBACK", "SPREAD", "TILT", "PAN", "DIFFUSE", "DRIFT", "WIDTH", "SYNC", "DUCK"},
        {"TIME", "FEEDBACK", "ERODE", "GRIT", "TONE", "DROPOUTS", "SPREAD", "WOBBLE", "SYNC", "DUCK", "HISS"},
    },
    {
        {0.55f, 0.45f, 0.75f, 0.55f, 0.6f, 0.4f, 0.3f, 0.35f, 0.f, 0.f, 0.f, 0.5258295f, 0.f},
        {0.66f, 0.45f, 0.15f, 0.35f, 0.f, 0.7f, 0.15f, 0.3f, 0.f, 0.4376181f, 0.5f},
        {0.6f, 0.5f, 0.7f, 0.45f, 0.3f, 0.6f, 0.3f, 0.f, 0.f, 0.f, 0.f, 0.4752678f},
        {0.6f, 0.45f, 0.9f, 0.3f, 0.25f, 0.35f, 0.25f, 0.5f, 0.f, 0.f, 0.5012200f, 0.5314789f, 0.5f},
        {0.55f, 0.55f, 0.f, 0.6f, 0.6f, 0.4f, 0.4f, 0.3f, 0.f, 0.f, 0.7737056f, 0.4376181f},
        {0.5f, 0.6f, 0.6f, 0.4f, 0.55f, 0.6f, 0.6f, 0.3f, 0.f, 0.5f, 0.5f},
        {0.35f, 0.35f, 0.3f, 0.5f, 0.2f, 0.6f, 0.9f, 0.5f, 0.3710943f, 0.f},
        {0.55f, 0.6f, 0.4f, 0.35f, 0.3f, 0.35f, 0.6f, 0.2f, 0.f, 0.f, 0.4731974f},
        {0.55f, 0.55f, 0.75f, 0.5f, 0.7f, 0.3f, 0.3f, 0.35f, 0.f, 0.f},
        {0.5f, 0.75f, 0.35f, 0.35f, 0.55f, 0.25f, 0.3f, 0.2f, 0.f, 0.f, 0.f},
    },
    1.f, /* LEVEL: unity */
    fmt,
};
