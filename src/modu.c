/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MOD: unusual modulation.
 *   TRIPLE      three resonant band-pass filters, in the manner of the Korg
 *               PS-3100 / 3200's resonators: each its own frequency, swept by
 *               the LFO a third of a cycle apart; the same LFO moves each one's
 *               pan, by the PAN LINK amount (negative: against the sweep)
 *   BARBERPOLE  an endless phaser: three phasers whose sweeps are a third
 *               apart and fade in and out, so the notches rise (or fall) forever
 *   SPIRAL      a Bode frequency shifter (Hilbert pair) inside a feedback
 *               delay: every repeat is shifted again, spiralling up or down
 *   CHOIR       a vowel-morphing formant bank (a-e-i-o-u) with a breathing LFO
 *               and a random drift: organic, chant-like
 *   DRIFT       a six-voice ensemble on wandering tape-like delays
 *   ORBIT       the sound circles you: Doppler, distance darkening, a
 *               counter-rotating satellite
 *   LIQUID      a photocell vibe: four all-passes on a lamp that glows and
 *               fades unevenly (the throb), chorus or vibrato
 *   TIDE        a harmonic tremolo that follows your playing: lows and highs
 *               pulse against each other and pan, faster (or deeper) as you
 *               play harder
 *   STRINGS     Juergen Haible's String Filter: forty fixed band-passes, one
 *               every ~2.6 semitones from 43 Hz to 14 kHz (the two boards'
 *               centre frequencies), odd bands left and even right, one Q for
 *               all, feedback round the bank held by a vactrol-like limiter,
 *               and a formant window scanning across the bands */
#include <stdio.h>

#include "modules.h"

int modu_init(Modu *m)
{
    memset(m, 0, sizeof *m);
    m->rng.s = 0x1234567u;
    for (int c = 0; c < 2; c++)
        if (dly_init(&m->sdl[c], TFX_SR * 3 / 2 + 64) || dly_init(&m->edl[c], TFX_SR / 10) ||
            dly_init(&m->odl[c], TFX_SR / 50))
            return -1;
    return 0;
}

void modu_free(Modu *m)
{
    for (int c = 0; c < 2; c++) dly_free(&m->sdl[c]), dly_free(&m->edl[c]), dly_free(&m->odl[c]);
}

void modu_reset(Modu *m, int algo)
{
    (void)algo;
    Dly keep_s[2], keep_e[2], keep_o[2];
    memcpy(keep_s, m->sdl, sizeof keep_s); memcpy(keep_e, m->edl, sizeof keep_e); memcpy(keep_o, m->odl, sizeof keep_o);
    uint32_t seed = m->rng.s;
    memset(m, 0, sizeof *m);
    memcpy(m->sdl, keep_s, sizeof keep_s); memcpy(m->edl, keep_e, sizeof keep_e); memcpy(m->odl, keep_o, sizeof keep_o);
    for (int c = 0; c < 2; c++) dly_clear(&m->sdl[c]), dly_clear(&m->edl[c]), dly_clear(&m->odl[c]);
    m->rng.s = seed ? seed : 0x1234567u;
    for (int i = 0; i < DRIFT_VOICES; i++) m->eph[i] = (float)i / DRIFT_VOICES;
}

static float bipolar_rate(float v, float lo, float hi)
{
    float s = v * 2.f - 1.f;
    if (fabsf(s) < 0.04f) return 0.f;
    float a = (fabsf(s) - 0.04f) / 0.96f;
    return (s < 0 ? -1.f : 1.f) * expmap(a, lo, hi);
}

/* ---------------------------------------------------------------- TRIPLE */
enum { SH_SINE, SH_TRI, SH_SQUARE, SH_SH, SH_DRIFT, SH_N };
static const char *const SHAPE[SH_N] = {"SINE", "TRIANGLE", "SQUARE", "S&H", "DRIFT"};

static float lfo_shape(Modu *m, int k, int shape, float ph)
{
    switch (shape) {
    case SH_TRI: return 4.f * fabsf(ph - floorf(ph + 0.5f)) - 1.f;
    case SH_SQUARE: { float s = lfo_sin(ph) * 6.f; return clampf(s, -1.f, 1.f); } /* softened edges */
    case SH_SH: return m->rsh[k];
    case SH_DRIFT: return wander_tick(&m->rw[k], &m->rng, 0.f);
    default: return lfo_sin(ph);
    }
}

static void triple(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: PHASE how far apart the three LFOs run (a third of a cycle at 2/3), SYNC, ENV>FREQ the
     * playing level moving all three, CARDS how differently the three cards are voiced, DIODES how
     * soon the resonance is held back, WIDTH where they sit, RESPONSE band / low / high pass */
    float phs = p[8] * 0.5f, envf = (p[10] * 2.f - 1.f) * 3.f, cards = p[11] * 2.f;
    float lim = 0.9f * exp2f((0.5f - p[12]) * 4.f), width = p[13];
    int resp = pstep(raw[14], 3);
    float f0[3] = {expmap(p[0], 60.f, 6000.f), expmap(p[1], 60.f, 6000.f), expmap(p[2], 60.f, 6000.f)};
    float Q = expmap(p[3], 1.5f, 40.f), k = 1.f / Q;
    float rate = sync_hz(raw[9], pt->bpm, expmap(p[4], 0.03f, 12.f)), depth = p[5] * 3.f, link = p[6] * 2.f - 1.f;
    int shape = pstep(raw[7], SH_N);
    float gain = 2.f + Q * 0.2f;
    float g[3] = {0}, pl[3] = {0}, pr[3] = {0};
    static const float HOME[3] = {-0.8f, 0.f, 0.8f}; /* the three sit left, centre, right */
    for (int i = 0; i < n; i++) {
        float prev = m->rph;
        m->rph += rate / TFX_SR;
        if (m->rph >= 1.f) m->rph -= 1.f;
        for (int j = 0; j < 3; j++) {
            /* sample-and-hold: a new value as each resonator's phase wraps, glided a little */
            float a = prev + j * phs, b = m->rph + j * phs;
            if (floorf(b) != floorf(a) || (b < a)) m->rsh_t[j] = rng_bi(&m->rng);
            m->rsh[j] += (m->rsh_t[j] - m->rsh[j]) * 0.004f;
            if (shape == SH_DRIFT) wander_tick(&m->rw[j], &m->rng, rate * 1.5f); /* advance the walk */
        }
        {
            float lev = 0.5f * (fabsf(l[i]) + fabsf(r[i]));
            m->renv += (lev - m->renv) * (lev > m->renv ? 0.004f : 0.0003f);
        }
        if ((i & 15) == 0) {
            float ev = envf * clampf(m->renv * 4.f, 0.f, 1.f);
            for (int j = 0; j < 3; j++) {
                float ph = m->rph + j * phs;
                float v = shape == SH_DRIFT ? wander_tick(&m->rw[j], &m->rng, 0.f) : lfo_shape(m, j, shape, ph);
                g[j] = svf_g_fast(clampf(f0[j] * exp2f(depth * v + ev), 30.f, 13000.f));
                /* as on the PS-3100: the LFO that moves a resonator's frequency also moves its
                 * place in the stereo field, by a variable (and reversible) amount */
                float pos = clampf(HOME[j] * width * (1.f - fabsf(link)) + link * v, -1.f, 1.f);
                pl[j] = sqrtf(0.5f * (1.f - pos));
                pr[j] = sqrtf(0.5f * (1.f + pos));
            }
        }
        float x = 0.5f * (l[i] + r[i]), ol = 0.f, orr = 0.f;
        for (int j = 0; j < 3; j++) {
            /* the PS-3200 card's KORG-35 filters: resonance held back by diodes (a soft
             * limit on the resonant state), and each card voiced a little differently by
             * its COLOR resistor (120k / 100k / 82k: the damping ratios here) */
            static const float COL[3] = {100.f / 120.f, 1.f, 100.f / 82.f};
            float kj = k * powf(COL[j], cards);
            SvfOut so = svf_run(&m->rs[j], x, g[j], kj);
            float y = kj * (resp == 0 ? so.bp : resp == 1 ? so.lp : so.hp) * gain;
            m->rs[j].ic1 = lim * softclip(m->rs[j].ic1 * (1.f / lim));
            ol += pl[j] * y;
            orr += pr[j] * y;
        }
        l[i] = ol;
        r[i] = orr;
    }
}

/* ---------------------------------------------------------------- BARBERPOLE */
static void barber(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC (one rise a bar / note value, in SPEED's direction), DRIFT RATE, FB TONE */
    static const int STG[4] = {4, 6, 8, 12};
    float rate = bipolar_rate(p[0], 0.01f, 1.5f);
    if (pstep(raw[8], NSYNC)) rate = (rate < 0.f ? -1.f : 1.f) * sync_hz(raw[8], pt->bpm, 0.f);
    float drate = expmap(p[9], 0.05f, 3.f), fbk = p[10] > 0.99f ? 1.f : onepole_coef(expmap(p[10], 300.f, 18000.f));
    float fb = p[1] * 0.85f, oct = 2.f + p[2] * 6.f, stereo = p[3] * 0.5f;
    int stages = STG[pstep(raw[4], 4)];
    float centre = expmap(p[5], 80.f, 1500.f), drift = p[6] * 0.15f, notch = p[7];
    for (int i = 0; i < n; i++) {
        m->bph += rate / TFX_SR;
        m->bph -= floorf(m->bph);
        float dv = wander_tick(&m->bw, &m->rng, drate) * drift;
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float acc = 0.f;
            for (int k = 0; k < BARBER_VOICES; k++) {
                float ph = m->bph + (float)k / BARBER_VOICES + (c ? stereo : 0.f) + dv;
                ph -= floorf(ph);
                if ((i & 7) == 0) {
                    float f = clampf(centre * exp2f(oct * (ph - 0.5f)), 20.f, 18000.f);
                    float t = svf_g_fast(f);
                    m->bcoef[c][k] = (t - 1.f) / (t + 1.f);
                }
                float a = m->bcoef[c][k];
                float y = x[c] + fb * m->bfb[c][k];
                float *z = m->bz[c][k];
                for (int s = 0; s < stages; s++) {
                    float o = a * y + z[s];
                    z[s] = y - a * o;
                    y = o;
                }
                m->bfl[c][k] += fbk * (y - m->bfl[c][k]);
                m->bfb[c][k] = m->bfl[c][k];
                float w = 0.5f - 0.5f * lfo_sin(ph + 0.25f); /* sin^2: in and out across its sweep */
                acc += w * y;
            }
            acc *= (1.f / 1.5f) * (1.f - 0.4f * fb);
            float o = lerpf(acc, 0.5f * (x[c] + acc) * 1.4f, notch);
            if (c) r[i] = o; else l[i] = o;
        }
    }
}

/* ---------------------------------------------------------------- SPIRAL */
static const float HA[4] = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
static const float HB[4] = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};

static inline void hilbert(Modu *m, int c, float x, float *re, float *im)
{
    float a = x, b = x;
    for (int s = 0; s < 4; s++) {
        float *xa = m->hx[c][0][s], *ya = m->hy[c][0][s];
        float ka = HA[s] * HA[s];
        float o = ka * (a + ya[1]) - xa[1];
        xa[1] = xa[0]; xa[0] = a; ya[1] = ya[0]; ya[0] = o; a = o;
        float *xb = m->hx[c][1][s], *yb = m->hy[c][1][s];
        float kb = HB[s] * HB[s];
        o = kb * (b + yb[1]) - xb[1];
        xb[1] = xb[0]; xb[0] = b; yb[1] = yb[0]; yb[0] = o; b = o;
    }
    *re = m->hd[c]; m->hd[c] = a; /* path A one sample late */
    *im = b;
}

static void spiral(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC (the wobble LFO), LOW CUT in the loop, SATURATE the loop */
    float lck = onepole_coef(expmap(p[9], 4.f, 800.f)), sat = expmap(p[10], 1.f, 8.f);
    float s = p[0] * 2.f - 1.f;
    float shift = (s < 0 ? -1.f : 1.f) * 1200.f * fabsf(s) * fabsf(s) * fabsf(s);
    float fb = p[1] * 0.97f, dtime = expmap(p[2], 0.005f, 1.5f) * TFX_SR;
    float spread = p[3], lrate = sync_hz(raw[8], pt->bpm, expmap(p[4], 0.02f, 6.f)), ldepth = p[5];
    float lpk = onepole_coef(expmap(p[6], 300.f, 16000.f)), ring = p[7];
    for (int i = 0; i < n; i++) {
        m->slfo += lrate / TFX_SR;
        m->slfo -= floorf(m->slfo);
        float wob = lfo_sin(m->slfo) * ldepth * (fabsf(shift) * 0.6f + 6.f);
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float hz = (shift + wob) * (c ? 1.f - 2.f * spread : 1.f);
            m->sph[c] += hz / TFX_SR;
            m->sph[c] -= floorf(m->sph[c]);
            float back = dly_lin(&m->sdl[c], dtime);
            float v = x[c] + fb * back, re, im;
            hilbert(m, c, v, &re, &im);
            float co = lfo_sin(m->sph[c] + 0.25f), si = lfo_sin(m->sph[c]);
            float ssb = re * co - im * si;
            float o = lerpf(ssb, re * co * 1.4f, ring);
            /* the loop: darkened, DC-blocked, soft-limited */
            m->slp[c] += lpk * (o - m->slp[c]);
            m->sfb_dc[c] += lck * (m->slp[c] - m->sfb_dc[c]);
            dly_push(&m->sdl[c], softclip((m->slp[c] - m->sfb_dc[c]) * sat) / sat);
            if (c) r[i] = o; else l[i] = o;
        }
    }
}

/* ---------------------------------------------------------------- CHOIR */
static const float VF[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
static const float VG[5][3] = {{1.f, 0.5f, 0.25f}, {1.f, 0.4f, 0.25f}, {1.f, 0.3f, 0.2f}, {1.f, 0.6f, 0.2f}, {1.f, 0.35f, 0.12f}};

static void choir(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, DRIFT RATE, HIGH FORMANT (the third formant's level), BREATH (noise, following
     * the playing, through the formants: a whisper) */
    float drate = expmap(p[9], 0.05f, 4.f), f3 = p[10] * 2.f, breath = p[11] * p[11] * 0.5f;
    float base = p[0] * 4.f, rate = sync_hz(raw[8], pt->bpm, expmap(p[1], 0.02f, 4.f)), depth = p[2] * 2.f;
    float Q = expmap(p[3], 2.f, 30.f), k = 1.f / Q, scale = expmap(p[4], 0.6f, 1.6f);
    float drift = p[5] * 1.5f, drive = 1.f + p[6] * p[6] * 10.f, stereo = p[7] * 1.5f;
    float gain = (0.9f + Q * 0.12f) / (0.7f + 0.3f * drive);
    float g[2][3] = {{0}}, fg[2][3] = {{0}};
    for (int i = 0; i < n; i++) {
        m->fph += rate / TFX_SR;
        m->fph -= floorf(m->fph);
        if ((i & 15) == 0) {
            for (int c = 0; c < 2; c++) {
                float pos = base + depth * lfo_sin(m->fph + (c ? 0.25f * p[7] : 0.f)) +
                            drift * wander_tick(&m->fw[c], &m->rng, drate * 16.f) + (c ? stereo : 0.f);
                /* fold into 0..4 (a .. u and back) */
                pos = fmodf(fabsf(pos), 8.f);
                if (pos > 4.f) pos = 8.f - pos;
                int a = (int)pos; if (a > 3) a = 3;
                float t = pos - (float)a;
                for (int f = 0; f < 3; f++) {
                    g[c][f] = svf_g_fast(lerpf(VF[a][f], VF[a + 1][f], t) * scale);
                    fg[c][f] = lerpf(VG[a][f], VG[a + 1][f], t) * (f == 2 ? f3 : 1.f);
                }
            }
        }
        float x[2] = {l[i], r[i]};
        float lev = 0.5f * (fabsf(x[0]) + fabsf(x[1]));
        m->cenv += (lev - m->cenv) * (lev > m->cenv ? 0.003f : 0.0004f);
        for (int c = 0; c < 2; c++) {
            float in = drive > 1.01f ? softclip(x[c] * drive) : x[c];
            if (breath > 0.f) in += breath * m->cenv * 4.f * rng_bi(&m->rng);
            float acc = 0.f;
            for (int f = 0; f < 3; f++) acc += fg[c][f] * k * svf_run(&m->fs[c][f], in, g[c][f], k).bp;
            acc *= gain;
            if (c) r[i] = acc; else l[i] = acc;
        }
    }
}

/* ---------------------------------------------------------------- DRIFT */
static const float ERATE[DRIFT_VOICES] = {1.f, 1.13f, 0.87f, 1.31f, 0.71f, 1.53f};

static void drift(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, SPREAD RATES (how unlike the voices' speeds are), CROSS (the loop fed across) */
    float rsp = p[9] * 2.f, cross = p[10];
    float depth = p[0] * 0.010f * TFX_SR, rate = sync_hz(raw[8], pt->bpm, expmap(p[1], 0.05f, 6.f));
    int nv = 2 + pstep(raw[2], 5);
    float spread = p[3], rnd = p[4], fb = p[5] * 0.7f, lpk = onepole_coef(expmap(p[6], 800.f, 18000.f));
    float base = expmap(p[7], 0.004f, 0.030f) * TFX_SR;
    float norm = 1.4f / sqrtf((float)nv);
    float gl[DRIFT_VOICES], gr[DRIFT_VOICES];
    for (int v = 0; v < nv; v++) {
        float pos = nv > 1 ? ((float)v / (nv - 1) * 2.f - 1.f) * spread : 0.f;
        if (v & 1) pos = -pos;
        gl[v] = sqrtf(0.5f * (1.f - pos)) * norm;
        gr[v] = sqrtf(0.5f * (1.f + pos)) * norm;
    }
    for (int i = 0; i < n; i++) {
        dly_push(&m->edl[0], l[i] + fb * m->efb[0]);
        dly_push(&m->edl[1], r[i] + fb * m->efb[1]);
        float ol = 0.f, orr = 0.f;
        for (int v = 0; v < nv; v++) {
            float er = 1.f + (ERATE[v] - 1.f) * rsp;
            m->eph[v] += rate * er / TFX_SR;
            m->eph[v] -= floorf(m->eph[v]);
            float mo = lerpf(lfo_sin(m->eph[v]), wander_tick(&m->ew[v], &m->rng, rate * er * 1.7f), rnd);
            float y = dly_herm(&m->edl[v & 1], base + depth * (0.5f + 0.5f * mo));
            ol += gl[v] * y;
            orr += gr[v] * y;
        }
        m->elp[0] += lpk * (ol - m->elp[0]);
        m->elp[1] += lpk * (orr - m->elp[1]);
        m->efb[0] = softclip(lerpf(m->elp[0], m->elp[1], cross)) * 0.7f; /* cross-fed: the ensemble widens as it recirculates */
        m->efb[1] = softclip(lerpf(m->elp[1], m->elp[0], cross)) * 0.7f;
        l[i] = m->elp[0];
        r[i] = m->elp[1];
    }
}

/* ---------------------------------------------------------------- ORBIT */
static void orbit(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC (a lap a bar / note value, in SPEED's direction), DARKEST (how dark far away gets),
     * SAT RATIO (the satellite's speed against the main one) */
    float dark = expmap(p[9], 200.f, 4000.f), sratio = -expmap(p[10], 0.25f, 4.f);
    float rate = bipolar_rate(p[0], 0.02f, 6.f), rad = p[1], ecc = 0.2f + 0.8f * p[2];
    if (pstep(raw[8], NSYNC)) rate = (rate < 0.f ? -1.f : 1.f) * sync_hz(raw[8], pt->bpm, 0.f);
    float sat = p[3], dist = p[4], width = p[5], wob = p[6];
    float dmax = 0.0035f * TFX_SR * rad, haas = 0.0007f * TFX_SR * width;
    for (int i = 0; i < n; i++) {
        float jit = 1.f + wob * wander_tick(&m->ow, &m->rng, 0.7f);
        m->oph += rate * jit / TFX_SR;
        m->oph -= floorf(m->oph);
        dly_push(&m->odl[0], 0.5f * (l[i] + r[i]));
        float out[2] = {0, 0};
        for (int s = 0; s < 2; s++) {
            if (s == 1 && sat < 0.01f) break;
            float th = s ? sratio * m->oph + 0.5f : m->oph;
            float x = lfo_sin(th + 0.25f), y = ecc * lfo_sin(th);
            float lev = s ? sat : 1.f;
            for (int c = 0; c < 2; c++) {
                float ex = c ? 1.f : -1.f;
                float d = sqrtf((x - ex) * (x - ex) + y * y) * 0.5f; /* 0..~1.1 */
                float dl = 0.002f * TFX_SR + dmax * d + haas * (0.5f + 0.5f * x * ex);
                float pan = sqrtf(clampf(0.5f * (1.f + x * ex), 0.f, 1.f));
                out[c] += lev * (0.35f + 0.65f * pan) * dly_herm(&m->odl[0], dl);
            }
        }
        /* behind you and far away: darker */
        float fx = lfo_sin(m->oph);
        float cut = expmap(clampf(1.f - dist * (0.6f - 0.4f * fx), 0.f, 1.f), dark, 18000.f);
        float k = onepole_coef(cut);
        m->olp[0] += k * (out[0] - m->olp[0]);
        m->olp[1] += k * (out[1] - m->olp[1]);
        l[i] = m->olp[0] * 1.2f;
        r[i] = m->olp[1] * 1.2f;
    }
}

/* ---------------------------------------------------------------- LIQUID */
static const float VRATIO[4] = {1.f, 1.65f, 2.7f, 4.4f}; /* four photocells, never matched */

static void liquid(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, CELLS (how unmatched the four photocells are), LAMP SPEED (how fast it glows and fades) */
    float cells = p[9] * 2.f, lamp = expmap(p[10], 0.25f, 4.f);
    float rate = sync_hz(raw[8], pt->bpm, expmap(p[0], 0.1f, 10.f)), depth = p[1], throb = p[2], vib = p[3];
    float centre = expmap(p[4], 150.f, 1200.f), fb = p[5] * 0.6f, stereo = p[6] * 0.5f, warm = p[7];
    float wk = onepole_coef(expmap(1.f - warm, 3000.f, 18000.f)), drive = 1.f + warm * 2.f;
    for (int i = 0; i < n; i++) {
        m->vph += rate / TFX_SR;
        m->vph -= floorf(m->vph);
        for (int c = 0; c < 2; c++) {
            /* the lamp: a pulse shaped by THROB, which the cells follow fast up and slow down */
            float ph = m->vph + (c ? stereo : 0.f);
            float s = 0.5f + 0.5f * lfo_sin(ph);
            float drv = powf(s, 1.f + throb * 3.f);
            float k = drv > m->vlamp[c] ? 0.004f : 0.0008f + 0.0015f * (1.f - throb);
            m->vlamp[c] += (drv - m->vlamp[c]) * fminf(k * (48000.f / TFX_SR) * 4.f * lamp, 0.5f);
            if ((i & 7) == 0)
                for (int st = 0; st < 4; st++) {
                    float f = centre * powf(VRATIO[st], cells) * exp2f(depth * 3.f * (m->vlamp[c] - 0.5f));
                    float t = svf_g_fast(f);
                    m->vco[c][st] = (t - 1.f) / (t + 1.f);
                }
        }
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float in = softclip(x[c] * drive) / drive * (1.f + 0.2f * warm);
            float y = in + fb * m->vfb[c];
            for (int st = 0; st < 4; st++) {
                float a = m->vco[c][st];
                float o = a * y + m->vz[c][st];
                m->vz[c][st] = y - a * o;
                y = o;
            }
            m->vfb[c] = y;
            float o = lerpf(0.5f * (in + y) * 1.3f, y, vib); /* chorus (with dry) .. vibrato (all-pass only) */
            m->vwarm[c] += wk * (o - m->vwarm[c]);
            if (c) r[i] = m->vwarm[c]; else l[i] = m->vwarm[c];
        }
    }
}

/* ---------------------------------------------------------------- TIDE */
static void tide(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SYNC, ENV SPEED (how fast the playing level is followed), SLOPE (the crossover's
     * steepness: 6 or 12 dB an octave) */
    float es = expmap(p[9], 0.25f, 4.f);
    int steep = pstep(raw[10], 2);
    float rate = sync_hz(raw[8], pt->bpm, expmap(p[0], 0.1f, 12.f)), depth = p[1], envr = p[2] * 2.f - 1.f, harm = p[3];
    float xk = onepole_coef(expmap(p[4], 200.f, 2000.f)), shape = p[5], pan = p[6], envd = p[7] * 2.f - 1.f;
    for (int i = 0; i < n; i++) {
        float lev = 0.5f * (fabsf(l[i]) + fabsf(r[i]));
        m->tenv += (lev - m->tenv) * fminf((lev > m->tenv ? 0.005f : 0.0002f) * es, 0.5f);
        float e = clampf(m->tenv * 4.f, 0.f, 1.f);
        /* playing harder pushes the tide faster (or slower), deeper (or shallower) */
        float rt = rate * exp2f(envr * 2.f * e);
        float dp = clampf(depth * (1.f + envd * e), 0.f, 1.f);
        m->tph += rt / TFX_SR;
        m->tph -= floorf(m->tph);
        float w = lfo_sin(m->tph);
        w = lerpf(w, clampf(w * 4.f, -1.f, 1.f), shape); /* sine .. square-ish */
        float a = 1.f - dp * (0.5f + 0.5f * w), b = 1.f - dp * (0.5f - 0.5f * w);
        float pl = 1.f - pan * 0.5f * (1.f + w), pr = 1.f - pan * 0.5f * (1.f - w);
        float x[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            m->tlo[c] += xk * (x[c] - m->tlo[c]);
            m->tlo2[c] += xk * (m->tlo[c] - m->tlo2[c]);
            float lo = steep ? m->tlo2[c] : m->tlo[c], hi = x[c] - lo;
            /* HARMONIC 0: the whole sound pulses; 1: lows and highs pulse against each other */
            float y = lo * a + hi * lerpf(a, b, harm);
            y *= c ? pr : pl;
            if (c) r[i] = y * 1.15f; else l[i] = y * 1.15f;
        }
    }
}

/* ---------------------------------------------------------------- STRINGS */
/* the centre frequencies printed on the two String Filter boards, merged in order:
 * PCB 2's bands fall between PCB 1's */
static const float SBF[STR_BANDS] = {
    43.5f, 50.4f, 58.4f, 67.8f, 78.5f, 91.1f, 106, 122, 142, 165, 191, 221, 256, 297, 345, 400, 463, 537, 623, 722,
    837, 970, 1130, 1300, 1510, 1750, 2030, 2360, 2730, 3170, 3670, 4260, 4930, 5720, 6630, 7690, 8914, 10340, 11980, 13890};

static void strings(Modu *m, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    /* DEEP: SCAN SYNC, SPLIT (which bands go left: odd / even as on the boards, lows / highs, or
     * every band in the middle), SCAN WIDTH, VAC ATTACK */
    int split = pstep(raw[9], 3);
    float swid = expmap(p[10], 2.f, 12.f), vatt = expmap(p[11], 0.25f, 4.f);
    float Q = expmap(p[0], 2.f, 80.f), k = 1.f / Q, fb = p[1] * 0.97f, tilt = p[2] * 2.f - 1.f, scan = p[3];
    float rate = sync_hz(raw[8], pt->bpm, expmap(p[4], 0.02f, 3.f)), stereo = p[5], warp = expmap(p[6], 0.5f, 2.f), vac = p[7];
    float norm = 3.6f / sqrtf(4.f + Q * 0.35f);
    /* coefficients: once a block (the bands do not move fast) */
    m->sscan += rate * (float)n / TFX_SR;
    m->sscan -= floorf(m->sscan);
    float centre = 0.5f + 0.45f * lfo_sin(m->sscan); /* where the scanning window sits, 0..1 across the bank */
    for (int b = 0; b < STR_BANDS; b++) {
        float f = SBF[b] * warp;
        m->sg[b] = f < 0.45f * TFX_SR ? svf_g(f) : 0.f;
        float pos = (float)b / (STR_BANDS - 1);
        float tl = exp2f(tilt * 2.5f * (pos - 0.5f) * 2.f);          /* +-5 dB across the bank, per side */
        float d = (pos - centre) * swid;
        float win = lerpf(1.f, 0.15f + 1.6f * expf(-d * d), scan);    /* SCAN: a formant window over the bands */
        m->sgain[b] = f < 0.45f * TFX_SR ? tl * win * norm : 0.f;
    }
    /* the vactrol: quick to light, slow to go dark (VTL5C3-like), more of it as VACTROL rises */
    float att = fminf(0.004f * vatt, 0.5f), rel = 0.00012f + 0.0004f * (1.f - vac), thr = 0.6f - 0.45f * vac;
    for (int i = 0; i < n; i++) {
        float in = 0.5f * (l[i] + r[i]);
        float lim = 1.f / (1.f + fmaxf(0.f, m->svac - thr) * 6.f);
        float x = in + fb * lim * 0.5f * (m->sfbk[0] + m->sfbk[1]);
        float ol = 0.f, orr = 0.f;
        for (int b = 0; b < STR_BANDS; b++) {
            if (m->sg[b] == 0.f) continue;
            float y = k * svf_run(&m->sb[b], x, m->sg[b], k).bp * m->sgain[b];
            /* odd bands to one side, even to the other: PCB 1 / PCB 2 */
            float side = split == 0 ? ((b & 1) ? 1.f : -1.f) : split == 1 ? (b < STR_BANDS / 2 ? -1.f : 1.f) : 0.f;
            ol += y * (1.f - stereo * side * 0.9f);
            orr += y * (1.f + stereo * side * 0.9f);
        }
        ol = softclip(ol);
        orr = softclip(orr);
        m->sfbk[0] = ol; m->sfbk[1] = orr;
        float e = fmaxf(fabsf(ol), fabsf(orr));
        m->svac += (e - m->svac) * (e > m->svac ? att : rel);
        l[i] = ol;
        r[i] = orr;
    }
}

void modu_process(Modu *m, int algo, const float *p, const float *raw, const Patch *pt, float *l, float *r, int n)
{
    switch (algo) {
    case MO_TRIPLE: triple(m, p, raw, pt, l, r, n); break;
    case MO_BARBER: barber(m, p, raw, pt, l, r, n); break;
    case MO_SPIRAL: spiral(m, p, raw, pt, l, r, n); break;
    case MO_CHOIR: choir(m, p, raw, pt, l, r, n); break;
    case MO_DRIFT: drift(m, p, raw, pt, l, r, n); break;
    case MO_ORBIT: orbit(m, p, raw, pt, l, r, n); break;
    case MO_LIQUID: liquid(m, p, raw, pt, l, r, n); break;
    case MO_TIDE: tide(m, p, raw, pt, l, r, n); break;
    default: strings(m, p, raw, pt, l, r, n); break;
    }
}

/* ---------------------------------------------------------------- the UI side */
static void fmt(int algo, int p, float v, const Patch *pt, char *o, int n)
{
    (void)pt;
    float s = v * 2.f - 1.f;
    if (p >= NBOARD) {
        int d = p - NBOARD;
        int is_sync = d == 0 && algo != MO_TRIPLE;
        if (is_sync || (algo == MO_TRIPLE && d == 1)) { snprintf(o, n, "%s", sync_name(v)); return; }
        switch (algo) {
        case MO_TRIPLE:
            if (d == 0) { snprintf(o, n, "%.0f deg", v * 180.f); return; }
            if (d == 2) { snprintf(o, n, "%+.1f oct", s * 3.f); return; }
            if (d == 3) { snprintf(o, n, v < 0.02f ? "MATCHED" : "x%.1f", v * 2.f); return; }
            if (d == 6) { static const char *const R[3] = {"BAND PASS", "LOW PASS", "HIGH PASS"}; snprintf(o, n, "%s", R[pstep(v, 3)]); return; }
            break;
        case MO_BARBER:
            if (d == 1) { snprintf(o, n, "%.2f Hz", expmap(v, 0.05f, 3.f)); return; }
            if (d == 2) { snprintf(o, n, v > 0.99f ? "OPEN" : "%.0f Hz", expmap(v, 300.f, 18000.f)); return; }
            break;
        case MO_SPIRAL:
            if (d == 1) { snprintf(o, n, "%.0f Hz", expmap(v, 4.f, 800.f)); return; }
            if (d == 2) { snprintf(o, n, "x%.1f", expmap(v, 1.f, 8.f)); return; }
            break;
        case MO_CHOIR:
            if (d == 1) { snprintf(o, n, "%.2f Hz", expmap(v, 0.05f, 4.f)); return; }
            if (d == 2) { snprintf(o, n, "x%.2f", v * 2.f); return; }
            break;
        case MO_DRIFT:
            if (d == 1) { snprintf(o, n, "x%.2f", v * 2.f); return; }
            break;
        case MO_ORBIT:
            if (d == 1) { snprintf(o, n, "%.0f Hz", expmap(v, 200.f, 4000.f)); return; }
            if (d == 2) { snprintf(o, n, "x%.2f", expmap(v, 0.25f, 4.f)); return; }
            break;
        case MO_LIQUID:
            if (d == 1) { snprintf(o, n, v < 0.02f ? "MATCHED" : "x%.1f", v * 2.f); return; }
            if (d == 2) { snprintf(o, n, "x%.2f", expmap(v, 0.25f, 4.f)); return; }
            break;
        case MO_TIDE:
            if (d == 1) { snprintf(o, n, "x%.2f", expmap(v, 0.25f, 4.f)); return; }
            if (d == 2) { snprintf(o, n, pstep(v, 2) ? "12 dB/oct" : "6 dB/oct"); return; }
            break;
        case MO_STRINGS:
            if (d == 1) { static const char *const S[3] = {"ODD L / EVEN R", "LOWS L / HIGHS R", "ALL CENTRE"}; snprintf(o, n, "%s", S[pstep(v, 3)]); return; }
            if (d == 2) { snprintf(o, n, "%.1f bands", 40.f / expmap(v, 2.f, 12.f)); return; }
            if (d == 3) { snprintf(o, n, "x%.2f", expmap(v, 0.25f, 4.f)); return; }
            break;
        }
        snprintf(o, n, "%.0f %%", v * 100.f);
        return;
    }
    switch (algo) {
    case MO_TRIPLE:
        if (p <= 2) { snprintf(o, n, "%.0f Hz", expmap(v, 60.f, 6000.f)); return; }
        if (p == 3) { snprintf(o, n, "Q %.1f", expmap(v, 1.5f, 40.f)); return; }
        if (p == 4) { snprintf(o, n, "%.2f Hz", expmap(v, 0.03f, 12.f)); return; }
        if (p == 5) { snprintf(o, n, "%.1f oct", v * 3.f); return; }
        if (p == 6) { float k = v * 2.f - 1.f; snprintf(o, n, fabsf(k) < 0.02f ? "FIXED" : "%+.0f %%", k * 100.f); return; }
        if (p == 7) { snprintf(o, n, "%s", SHAPE[pstep(v, SH_N)]); return; }
        break;
    case MO_BARBER:
        if (p == 0) { float h = bipolar_rate(v, 0.01f, 1.5f); snprintf(o, n, h == 0.f ? "STOP" : "%s %.2f Hz", h > 0 ? "UP" : "DOWN", fabsf(h)); return; }
        if (p == 2) { snprintf(o, n, "%.1f oct", 2.f + v * 6.f); return; }
        if (p == 4) { static const int S[4] = {4, 6, 8, 12}; snprintf(o, n, "%d", S[pstep(v, 4)]); return; }
        if (p == 5) { snprintf(o, n, "%.0f Hz", expmap(v, 80.f, 1500.f)); return; }
        break;
    case MO_SPIRAL:
        if (p == 0) { snprintf(o, n, "%+.1f Hz", (s < 0 ? -1.f : 1.f) * 1200.f * fabsf(s * s * s)); return; }
        if (p == 2) { float t = expmap(v, 0.005f, 1.5f); snprintf(o, n, t < 1.f ? "%.0f ms" : "%.2f s", t < 1.f ? t * 1000.f : t); return; }
        if (p == 4) { snprintf(o, n, "%.2f Hz", expmap(v, 0.02f, 6.f)); return; }
        if (p == 6) { snprintf(o, n, "%.0f Hz", expmap(v, 300.f, 16000.f)); return; }
        break;
    case MO_CHOIR:
        if (p == 0) { static const char *V[5] = {"A", "E", "I", "O", "U"}; float q = v * 4.f; int a = (int)q; if (a > 3) a = 3;
            snprintf(o, n, "%s>%s %.0f%%", V[a], V[a + 1], (q - a) * 100.f); return; }
        if (p == 1) { snprintf(o, n, "%.2f Hz", expmap(v, 0.02f, 4.f)); return; }
        if (p == 3) { snprintf(o, n, "Q %.1f", expmap(v, 2.f, 30.f)); return; }
        if (p == 4) { snprintf(o, n, "x%.2f", expmap(v, 0.6f, 1.6f)); return; }
        break;
    case MO_DRIFT:
        if (p == 0) { snprintf(o, n, "%.1f ms", v * 10.f); return; }
        if (p == 1) { snprintf(o, n, "%.2f Hz", expmap(v, 0.05f, 6.f)); return; }
        if (p == 2) { snprintf(o, n, "%d", 2 + pstep(v, 5)); return; }
        if (p == 6) { snprintf(o, n, "%.0f Hz", expmap(v, 800.f, 18000.f)); return; }
        if (p == 7) { snprintf(o, n, "%.1f ms", expmap(v, 4.f, 30.f)); return; }
        break;
    case MO_ORBIT:
        if (p == 0) { float h = bipolar_rate(v, 0.02f, 6.f); snprintf(o, n, h == 0.f ? "STILL" : "%s %.2f Hz", h > 0 ? "CW" : "CCW", fabsf(h)); return; }
        break;
    case MO_LIQUID:
        if (p == 0) { snprintf(o, n, "%.2f Hz", expmap(v, 0.1f, 10.f)); return; }
        if (p == 3) { snprintf(o, n, v < 0.05f ? "CHORUS" : v > 0.95f ? "VIBRATO" : "%.0f%% VIB", v * 100.f); return; }
        if (p == 4) { snprintf(o, n, "%.0f Hz", expmap(v, 150.f, 1200.f)); return; }
        break;
    case MO_STRINGS:
        if (p == 0) { snprintf(o, n, "Q %.1f", expmap(v, 2.f, 80.f)); return; }
        if (p == 2) { snprintf(o, n, "%+.0f", s * 100.f); return; }
        if (p == 4) { snprintf(o, n, "%.2f Hz", expmap(v, 0.02f, 3.f)); return; }
        if (p == 6) { snprintf(o, n, "x%.2f", expmap(v, 0.5f, 2.f)); return; }
        break;
    case MO_TIDE:
        if (p == 0) { snprintf(o, n, "%.2f Hz", expmap(v, 0.1f, 12.f)); return; }
        if (p == 2 || p == 7) { snprintf(o, n, "%+.0f %%", s * 100.f); return; }
        if (p == 4) { snprintf(o, n, "%.0f Hz", expmap(v, 200.f, 2000.f)); return; }
        break;
    }
    snprintf(o, n, "%.0f %%", v * 100.f);
}

const ModInfo modu_info = {
    "MOD",
    MO_N,
    {"TRIPLE", "BARBERPOLE", "SPIRAL", "CHOIR", "DRIFT", "ORBIT", "LIQUID", "TIDE", "STRINGS"},
    {"three resonant filters, LFO-swept and panned (PS-3100)",
     "endless phaser: notches rise or fall forever",
     "frequency shifter in a feedback loop: spiralling repeats",
     "vowel-morphing formant bank: chanting, breathing",
     "six wandering tape voices: an organic ensemble",
     "the sound circles you: Doppler, distance, a satellite",
     "photocell vibe: an uneven lamp, chorus or vibrato",
     "harmonic tremolo that speeds up as you play harder",
     "Haible String Filter: 40 bands, odd L / even R, fed back"},
    {
        {"FREQ 1", "FREQ 2", "FREQ 3", "RESO", "LFO RATE", "LFO DEPTH", "PAN LINK", "LFO SHAPE", "PHASE", "SYNC", "ENV>FREQ", "CARDS", "DIODES", "WIDTH", "RESPONSE"},
        {"SPEED", "RESO", "DEPTH", "STEREO", "STAGES", "CENTRE", "DRIFT", "NOTCH", "SYNC", "DRIFT RATE", "FB TONE"},
        {"SHIFT", "FEEDBACK", "TIME", "SPREAD", "LFO RATE", "LFO DEPTH", "TONE", "RING", "SYNC", "LOOP LOW CUT", "SATURATE"},
        {"VOWEL", "RATE", "DEPTH", "RESO", "THROAT", "DRIFT", "DRIVE", "STEREO", "SYNC", "DRIFT RATE", "HIGH FORMANT", "BREATH"},
        {"DEPTH", "RATE", "VOICES", "SPREAD", "WANDER", "FEEDBACK", "TONE", "BASE", "SYNC", "SPREAD RATES", "CROSS"},
        {"SPEED", "RADIUS", "SHAPE", "SATELLITE", "DISTANCE", "WIDTH", "WOBBLE", "", "SYNC", "DARKEST", "SAT RATIO"},
        {"SPEED", "INTENSITY", "THROB", "VIBRATO", "CENTRE", "FEEDBACK", "STEREO", "WARMTH", "SYNC", "CELLS", "LAMP SPEED"},
        {"RATE", "DEPTH", "ENV>RATE", "HARMONIC", "CROSSOVER", "SHAPE", "PAN", "ENV>DEPTH", "SYNC", "ENV SPEED", "SLOPE"},
        {"Q", "FEEDBACK", "SPECTRUM", "SCAN", "SCAN RATE", "STEREO", "WARP", "VACTROL", "SCAN SYNC", "SPLIT", "SCAN WIDTH", "VAC ATTACK"},
    },
    {
        {0.25f, 0.5f, 0.72f, 0.55f, 0.3f, 0.35f, 0.8f, 0.f, 0.6666667f, 0.f, 0.5f, 0.5f, 0.5f, 1.f, 0.f},
        {0.62f, 0.45f, 0.6f, 0.25f, 0.4f, 0.45f, 0.2f, 0.6f, 0.f, 0.4376181f, 1.f},
        {0.56f, 0.55f, 0.45f, 0.5f, 0.3f, 0.2f, 0.7f, 0.f, 0.f, 0.f, 0.f},
        {0.3f, 0.25f, 0.4f, 0.5f, 0.45f, 0.4f, 0.15f, 0.5f, 0.f, 0.4745388f, 0.5f, 0.f},
        {0.5f, 0.3f, 1.f, 0.8f, 0.5f, 0.2f, 0.7f, 0.5f, 0.f, 0.5f, 1.f},
        {0.62f, 0.5f, 0.6f, 0.3f, 0.5f, 0.6f, 0.2f, 0.f, 0.f, 0.5020734f, 0.6462406f},
        {0.35f, 0.6f, 0.5f, 0.f, 0.5f, 0.2f, 0.3f, 0.3f, 0.f, 0.5f, 0.5f},
        {0.4f, 0.6f, 0.75f, 1.f, 0.4f, 0.2f, 0.6f, 0.5f, 0.f, 0.5f, 0.75f},
        {0.55f, 0.35f, 0.5f, 0.3f, 0.25f, 0.85f, 0.5f, 0.5f, 0.f, 0.f, 0.5113916f, 0.5f},
    },
    0.6f,
    fmt,
};
