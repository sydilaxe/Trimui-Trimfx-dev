/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules.h"

#define TONE_LAT 64 /* >= tone_latency() */

typedef struct {
    int cur;            /* the algorithm running */
    float env;          /* its level: dips to 0 to change algorithm */
    float act;          /* 1 active .. 0 bypassed */
    float gate;         /* DELAY / REVERB: their input, 1 open .. 0 closed (trails) */
    float mix;
    float ps[NPARAM];   /* the running algorithm's parameters, glided */
    float pk;
} Slot;

struct Engine {
    Tone tone;
    Modu mo;
    Delay dl;
    Verb vb;
    Slot s[NMOD];
    float tdry[2][TONE_LAT];
    int tdw;
    float in_g, out_g, dry;
    float lim;          /* the safety limiter's gain */
    float in_pk[2], out_pk[2];
    int fresh;
};

static const int ORDERS[NORDER][3] = {
    {M_MOD, M_DELAY, M_VERB}, {M_MOD, M_VERB, M_DELAY}, {M_DELAY, M_MOD, M_VERB},
    {M_DELAY, M_VERB, M_MOD}, {M_VERB, M_MOD, M_DELAY}, {M_VERB, M_DELAY, M_MOD},
};
static const char *const ORDER_N[NORDER] = {"MOD>DLY>VERB", "MOD>VERB>DLY", "DLY>MOD>VERB",
                                            "DLY>VERB>MOD", "VERB>MOD>DLY", "VERB>DLY>MOD"};

const char *eng_order_name(int o) { return ORDER_N[o < 0 || o >= NORDER ? 0 : o]; }
void eng_order(int o, int out[3])
{
    if (o < 0 || o >= NORDER) o = 0;
    memcpy(out, ORDERS[o], sizeof ORDERS[o]);
}

const ModInfo *eng_info(int m)
{
    static const ModInfo *const I[NMOD] = {&tone_info, &modu_info, &delay_info, &verb_info};
    return I[m < 0 || m >= NMOD ? 0 : m];
}

/* ------------------------------------------------------------------ patches */
void patch_default(Patch *p)
{
    memset(p, 0, sizeof *p);
    for (int m = 0; m < NMOD; m++) {
        const ModInfo *in = eng_info(m);
        for (int a = 0; a < in->nalgo; a++)
            for (int k = 0; k < NPARAM; k++) p->m[m].p[a][k] = in->pdef[a][k];
        p->m[m].mix = in->mixdef;
    }
    p->bpm = 100.f;
    p->dry = 0.f;
}

static void setp(Patch *p, int m, int algo, float mix, const float *v)
{
    p->m[m].algo = algo;
    p->m[m].mix = mix;
    if (v)
        for (int k = 0; k < NBOARD; k++) /* the presets set the board's eight; DEEP stays at its defaults */
            if (v[k] >= 0.f) p->m[m].p[algo][k] = v[k];
}

const char *patch_factory(Patch *p, int i)
{
    patch_default(p);
    switch (i) {
    case 0: /* the default chain, voiced for a dreamy send */
        setp(p, M_TONE, 0, 1.f, NULL);
        setp(p, M_MOD, 0, 0.55f, NULL);
        setp(p, M_DELAY, 2, 0.5f, NULL);
        setp(p, M_VERB, 0, 0.65f, NULL);
        return "DREAM SEND";
    case 1:
        setp(p, M_TONE, 2, 1.f, (float[]){0.42f, 0.38f, 0.3f, -1, -1, -1, -1, 0.8f});
        setp(p, M_MOD, 1, 0.5f, (float[]){0.6f, 0.75f, 0.55f, 0.8f, -1, -1, -1, -1});
        setp(p, M_DELAY, 0, 0.6f, (float[]){0.62f, 0.6f, 0.85f, 0.65f, 0.65f, 0.6f, 0.4f, 0.6f});
        setp(p, M_VERB, 7, 1.f, (float[]){0.8f, 0.75f, 0.45f, 0.5f, 0.1f, 0.9f, 0.55f, 0.f});
        return "ASCENSION";
    case 2:
        setp(p, M_TONE, 3, 1.f, (float[]){0.35f, -1, 0.5f, 0.15f, -1, -1, -1, -1});
        setp(p, M_MOD, 0, 0.8f, (float[]){0.2f, 0.42f, 0.66f, 0.75f, 0.2f, 0.55f, 0.95f, 0.75f});
        setp(p, M_DELAY, 1, 0.45f, (float[]){0.7f, 0.5f, 0.5f, 0.3f, 0.25f, 0.9f, 0.3f, 0.5f});
        setp(p, M_VERB, 4, 0.55f, NULL);
        p->bpm = 92.f;
        return "PS RITUAL";
    case 3:
        setp(p, M_TONE, 0, 1.f, (float[]){0.6f, 0.25f, 0.6f, 0.f, -1, 0.45f, -1, -1});
        setp(p, M_MOD, 3, 0.6f, (float[]){0.65f, 0.25f, 1.f, 1.f, 0.8f, 0.3f, 0.55f, 0.6f});
        setp(p, M_DELAY, 3, 0.55f, (float[]){0.68f, 0.7f, 0.95f, 0.5f, 0.4f, 0.6f, 0.45f, 0.7f});
        setp(p, M_VERB, 8, 0.5f, NULL);
        return "DUST TAPE";
    case 4:
        setp(p, M_TONE, 2, 1.f, (float[]){0.5f, 0.62f, -1, -1, -1, -1, -1, -1});
        setp(p, M_MOD, 4, 0.7f, NULL);
        setp(p, M_DELAY, 2, 0.75f, (float[]){0.75f, 0.65f, 1.f, 0.6f, 0.4f, 0.45f, 0.5f, 0.55f});
        setp(p, M_VERB, 5, 1.f, (float[]){0.85f, 0.9f, 0.55f, -1, 0.15f, 0.4f, 0.6f, 1.f});
        return "BLOOM CATHEDRAL";
    case 5:
        setp(p, M_TONE, 1, 1.f, (float[]){-1, -1, 0.5f, 0.4f, -1, 0.55f, 0.3f, -1});
        setp(p, M_MOD, 2, 0.65f, NULL);
        setp(p, M_DELAY, 1, 0.5f, (float[]){0.5f, 0.55f, 0.85f, 0.65f, 0.3f, 0.8f, 0.2f, 0.f});
        setp(p, M_VERB, 3, 0.6f, NULL);
        p->bpm = 120.f;
        return "STATION CHANT";
    case 6:
        setp(p, M_TONE, 0, 1.f, NULL);
        setp(p, M_MOD, 5, 0.5f, NULL);
        setp(p, M_DELAY, 0, 1.f, (float[]){0.8f, 0.7f, 0.4f, 0.8f, 0.8f, 0.9f, 0.5f, 0.6f});
        setp(p, M_VERB, 2, 1.f, (float[]){0.85f, 0.9f, 0.5f, 0.6f, 0.f, 0.9f, 0.3f, 0.65f});
        return "FULL WET CLOUD";
    default:
        setp(p, M_TONE, 3, 1.f, (float[]){-1, -1, -1, 0.35f, -1, -1, -1, -1});
        setp(p, M_MOD, 1, 0.45f, (float[]){0.45f, 0.8f, 0.3f, 1.f, 0.6f, 0.5f, 0.5f, 0.3f});
        setp(p, M_DELAY, 2, 0.6f, (float[]){0.5f, 0.55f, 0.55f, 0.7f, 0.6f, 0.5f, 0.7f, 0.4f});
        setp(p, M_VERB, 9, 0.6f, NULL);
        return "MIRROR WORLD";
    }
}

/* ------------------------------------------------------------------ engine */
Engine *engine_new(void)
{
    Engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (tone_init(&e->tone) || modu_init(&e->mo) || delay_init(&e->dl) || verb_init(&e->vb)) {
        engine_free(e);
        return NULL;
    }
    engine_reset(e);
    return e;
}

void engine_free(Engine *e)
{
    if (!e) return;
    tone_free(&e->tone);
    modu_free(&e->mo);
    delay_free(&e->dl);
    verb_free(&e->vb);
    free(e);
}

void engine_reset(Engine *e)
{
    tone_reset(&e->tone, 0);
    modu_reset(&e->mo, 0);
    delay_reset(&e->dl, 0);
    verb_reset(&e->vb, 0);
    memset(e->s, 0, sizeof e->s);
    memset(e->tdry, 0, sizeof e->tdry);
    e->lim = 1.f;
    e->fresh = 1;
}

static void run_module(Engine *e, int m, int algo, float *l, float *r, const float *p, const float *raw, const Patch *pt, int n)
{
    switch (m) {
    case M_TONE: tone_process(&e->tone, algo, p, raw, pt, l, r, n); break;
    case M_MOD: modu_process(&e->mo, algo, p, raw, pt, l, r, n); break;
    case M_DELAY: delay_process(&e->dl, algo, p, raw, pt, l, r, n); break;
    default: verb_process(&e->vb, algo, p, raw, pt, l, r, n); break;
    }
}

static void reset_module(Engine *e, int m, int algo)
{
    switch (m) {
    case M_TONE: tone_reset(&e->tone, algo); break;
    case M_MOD: modu_reset(&e->mo, algo); break;
    case M_DELAY: delay_reset(&e->dl, algo); break;
    default: verb_reset(&e->vb, algo); break;
    }
}

/* one module over a block: its dry / wet mix, bypass, algorithm change */
static void slot_block(Engine *e, int m, const Patch *pt, float *l, float *r, int n)
{
    Slot *s = &e->s[m];
    const ModState *ms = &pt->m[m];
    int want = ms->algo;
    if (want < 0 || want >= eng_info(m)->nalgo) want = 0;
    const float *raw = ms->p[want];
    int trails = m == M_DELAY || m == M_VERB;
    float sr = (float)TFX_SR;

    if (e->fresh) {
        s->cur = want;
        s->env = 1.f;
        s->act = s->gate = ms->bypass ? 0.f : 1.f;
        s->mix = ms->mix;
        memcpy(s->ps, raw, sizeof s->ps);
        reset_module(e, m, want);
    }
    /* changing algorithm: fade out, start the new one clean, fade in (TONE dips by itself) */
    float env0 = s->env, env1;
    if (m == M_TONE && want < TN_CASSETTE && s->cur < TN_CASSETTE) {
        s->cur = want; /* between mastering chains SP-1 Master dips by itself */
        env1 = fminf(1.f, s->env + (float)n / (0.02f * sr));
    } else if (want != s->cur) {
        env1 = s->env - (float)n / (0.012f * sr);
        if (env1 <= 0.f) {
            env1 = 0.f;
            reset_module(e, m, want);
            s->cur = want;
            memcpy(s->ps, raw, sizeof s->ps);
        }
    } else
        env1 = fminf(1.f, s->env + (float)n / (0.02f * sr));
    s->env = env1;

    const float *rawc = ms->p[s->cur];
    float k = 1.f - expf(-(float)n / (0.025f * sr));
    for (int i = 0; i < NPARAM; i++) s->ps[i] += (rawc[i] - s->ps[i]) * k;
    float mix0 = s->mix;
    s->mix += (ms->mix - s->mix) * k;
    float act0 = s->act, act1 = clampf(s->act + (ms->bypass ? -1.f : 1.f) * (float)n / (0.02f * sr), 0.f, 1.f);
    s->act = act1;
    float gate0 = s->gate, gate1 = clampf(s->gate + (ms->bypass ? -1.f : 1.f) * (float)n / (0.01f * sr), 0.f, 1.f);
    s->gate = gate1;

    /* fully bypassed and nothing ringing: TONE and MOD need not run at all */
    if (!trails && act0 == 0.f && act1 == 0.f) {
        if (m == M_TONE) /* keep the dry line moving for when it comes back */
            for (int i = 0; i < n; i++) {
                e->tdry[0][e->tdw] = l[i]; e->tdry[1][e->tdw] = r[i];
                e->tdw = (e->tdw + 1) % TONE_LAT;
            }
        return;
    }

    float wl[MOD_BLOCK], wr[MOD_BLOCK], dl[MOD_BLOCK], dr[MOD_BLOCK];
    if (m == M_TONE) {
        int lat = tone_latency();
        for (int i = 0; i < n; i++) {
            e->tdry[0][e->tdw] = l[i]; e->tdry[1][e->tdw] = r[i];
            int rd = (e->tdw - lat + TONE_LAT) % TONE_LAT;
            dl[i] = e->tdry[0][rd]; dr[i] = e->tdry[1][rd];
            e->tdw = (e->tdw + 1) % TONE_LAT;
        }
    } else {
        memcpy(dl, l, sizeof(float) * n);
        memcpy(dr, r, sizeof(float) * n);
    }
    memcpy(wl, l, sizeof(float) * n);
    memcpy(wr, r, sizeof(float) * n);
    if (trails)
        for (int i = 0; i < n; i++) {
            float g = gate0 + (gate1 - gate0) * (float)i / (float)n;
            wl[i] *= g; wr[i] *= g;
        }
    run_module(e, m, s->cur, wl, wr, s->ps, rawc, pt, n);

    float dg0, wg0, dg1, wg1, pk = 0.f;
    mix_gains(mix0, &dg0, &wg0);
    mix_gains(s->mix, &dg1, &wg1);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        float dg = dg0 + (dg1 - dg0) * t, wg = (wg0 + (wg1 - wg0) * t) * (env0 + (env1 - env0) * t);
        float a = act0 + (act1 - act0) * t;
        float ol, orr;
        if (trails) {
            /* bypassed: all the dry, and the tail rings on */
            float d = 1.f + (dg - 1.f) * a;
            ol = dl[i] * d + wl[i] * wg;
            orr = dr[i] * d + wr[i] * wg;
        } else {
            ol = dl[i] + (dl[i] * dg + wl[i] * wg - dl[i]) * a;
            orr = dr[i] + (dr[i] * dg + wr[i] * wg - dr[i]) * a;
        }
        /* a guard against a module gone wrong: NaN or Inf out becomes silence */
        if (!(fabsf(ol) < 1e4f)) ol = 0.f;
        if (!(fabsf(orr) < 1e4f)) orr = 0.f;
        l[i] = ol; r[i] = orr;
        float q = fabsf(ol) > fabsf(orr) ? fabsf(ol) : fabsf(orr);
        if (q > pk) pk = q;
    }
    if (pk > s->pk) s->pk = pk;
}

static void block(Engine *e, const Patch *p, const float *in, float *out, int n)
{
    float l[MOD_BLOCK], r[MOD_BLOCK], il[MOD_BLOCK], ir[MOD_BLOCK];
    float ing = db2lin(clampf(p->in_db, -24.f, 24.f)), outg = db2lin(clampf(p->out_db, -24.f, 24.f));
    if (e->fresh) e->in_g = ing, e->out_g = outg, e->dry = p->dry;
    float ig0 = e->in_g, og0 = e->out_g, dr0 = e->dry;
    float k = 1.f - expf(-(float)n / (0.03f * TFX_SR));
    e->in_g += (ing - e->in_g) * k;
    e->out_g += (outg - e->out_g) * k;
    e->dry += (clampf(p->dry, 0.f, 1.f) - e->dry) * k;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n, g = ig0 + (e->in_g - ig0) * t;
        il[i] = in[2 * i]; ir[i] = in[2 * i + 1];
        l[i] = il[i] * g; r[i] = ir[i] * g;
        float a = fabsf(il[i]), b = fabsf(ir[i]);
        if (a > e->in_pk[0]) e->in_pk[0] = a;
        if (b > e->in_pk[1]) e->in_pk[1] = b;
    }
    slot_block(e, M_TONE, p, l, r, n);
    int ord[3];
    eng_order(p->order, ord);
    for (int j = 0; j < 3; j++) slot_block(e, ord[j], p, l, r, n);
    e->fresh = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        float og = og0 + (e->out_g - og0) * t, dg = dr0 + (e->dry - dr0) * t;
        float a = l[i] * og + il[i] * dg, b = r[i] * og + ir[i] * dg;
        /* the safety limiter: instant down to keep the peak near -1 dBFS, back up over ~0.3 s,
         * then a soft clip for what slips past */
        float pk = fmaxf(fabsf(a), fabsf(b)) * e->lim;
        if (pk > 0.89f) e->lim *= 0.89f / pk;
        else e->lim += (1.f - e->lim) * 0.00007f;
        a = softclip(a * e->lim); b = softclip(b * e->lim);
        out[2 * i] = a; out[2 * i + 1] = b;
        if (fabsf(a) > e->out_pk[0]) e->out_pk[0] = fabsf(a);
        if (fabsf(b) > e->out_pk[1]) e->out_pk[1] = fabsf(b);
    }
}

void engine_process(Engine *e, const Patch *p, const float *in, float *out, int n)
{
    while (n > 0) {
        int c = n > MOD_BLOCK ? MOD_BLOCK : n;
        block(e, p, in, out, c);
        in += 2 * c; out += 2 * c; n -= c;
    }
}

void engine_meters(Engine *e, EngMeters *m)
{
    for (int c = 0; c < 2; c++) {
        m->in_pk[c] = e->in_pk[c]; e->in_pk[c] = 0.f;
        m->out_pk[c] = e->out_pk[c]; e->out_pk[c] = 0.f;
    }
    for (int k = 0; k < NMOD; k++) {
        m->mod_pk[k] = e->s[k].pk; e->s[k].pk = 0.f;
        m->algo_now[k] = e->s[k].cur;
    }
    m->gr_db = tone_gr_db(&e->tone);
}
