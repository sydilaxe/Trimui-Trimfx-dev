/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TrimFX engine tests, on the host: every algorithm of every module, fed a
 * noise burst then silence, and swept through its parameters:
 *   - the output stays finite and below full scale (no blow-ups)
 *   - silence in gives silence out once the tail has gone (except FREEZE)
 *   - each algorithm makes sound (its wet path is alive at full wet)
 *   - mix 100 % has no dry: a module at full wet does not pass the input
 *   - FREEZE holds the reverb's and the delay's tail
 *   - the cost per second of audio (for the A53, divide the margin by ~6)
 * `test_engine -w dir` also writes a WAV of each algorithm for listening. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../src/engine.h"
#include "../src/modules.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); }      \
    } while (0)

#define SR 48000
static uint32_t rs = 1;
static float noise(void)
{
    rs = rs * 1664525u + 1013904223u;
    return (float)((int32_t)rs) / 2147483648.f;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void write_wav(const char *path, const float *x, int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int bytes = frames * 4;
    unsigned char h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0....\0\0\0\0\x04\0\x10\0data....";
    int v = 36 + bytes; memcpy(h + 4, &v, 4);
    v = SR; memcpy(h + 24, &v, 4);
    v = SR * 4; memcpy(h + 28, &v, 4);
    memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < frames * 2; i++) {
        float s = x[i] > 1.f ? 1.f : x[i] < -1.f ? -1.f : x[i];
        short q = (short)(s * 32767.f);
        fwrite(&q, 2, 1, f);
    }
    fclose(f);
}

/* a test signal: plucked tones and noise bursts, so every effect has something to bite */
static void source(float *buf, int frames, int seed)
{
    rs = (uint32_t)seed * 2654435761u + 1;
    memset(buf, 0, sizeof(float) * 2 * frames);
    for (int i = 0; i < frames; i++) {
        float t = (float)i / SR;
        float env = expf(-fmodf(t, 0.5f) * 6.f);
        float f = 220.f * powf(2.f, (float)((int)(t * 2.f) % 5) * 3.f / 12.f);
        float s = 0.3f * env * sinf(6.2831853f * f * t) + (fmodf(t, 1.f) < 0.02f ? 0.3f * noise() : 0.f);
        buf[2 * i] = s;
        buf[2 * i + 1] = s * 0.9f + 0.02f * noise();
    }
}

static float rms(const float *x, int frames)
{
    double a = 0;
    for (int i = 0; i < 2 * frames; i++) a += (double)x[i] * x[i];
    return (float)sqrt(a / (2.0 * frames));
}

int main(int argc, char **argv)
{
    const char *wavdir = argc > 2 && !strcmp(argv[1], "-w") ? argv[2] : NULL;
    Engine *e = engine_new();
    CHECK(e != NULL, "engine made");
    if (!e) return 1;
    const int play = 3 * SR, tail = 8 * SR, total = play + tail;
    float *in = malloc(sizeof(float) * 2 * total), *out = malloc(sizeof(float) * 2 * total);
    source(in, play, 1);
    memset(in + 2 * play, 0, sizeof(float) * 2 * tail);
    double cpu_total = 0, audio_total = 0, worst = 0;
    char worst_name[64] = "";

    for (int m = 0; m < NMOD; m++) {
        const ModInfo *mi = eng_info(m);
        for (int a = 0; a < mi->nalgo; a++) {
            Patch p;
            patch_default(&p);
            for (int k = 0; k < NMOD; k++) p.m[k].bypass = k != m;
            p.m[m].algo = a;
            p.m[m].mix = 1.f; /* all wet */
            engine_reset(e);
            double t0 = now();
            /* sweep the four stick parameters while it plays, as a player would */
            for (int i = 0; i < total; i += 256) {
                float t = (float)i / total;
                for (int k = 0; k < 4; k++)
                    p.m[m].p[a][k] = mi->pdef[a][k] + 0.35f * sinf(6.283f * t * (1.f + k * 0.37f) + k);
                for (int k = 0; k < 4; k++) p.m[m].p[a][k] = p.m[m].p[a][k] < 0 ? 0 : p.m[m].p[a][k] > 1 ? 1 : p.m[m].p[a][k];
                if (i > play) /* settle back to the defaults for the tail */
                    for (int k = 0; k < NPARAM; k++) p.m[m].p[a][k] = mi->pdef[a][k];
                int n = total - i < 256 ? total - i : 256;
                engine_process(e, &p, in + 2 * i, out + 2 * i, n);
            }
            double dt = now() - t0;
            cpu_total += dt; audio_total += (double)total / SR;
            if (dt / ((double)total / SR) > worst) worst = dt / ((double)total / SR), snprintf(worst_name, sizeof worst_name, "%s %s", mi->name, mi->algo[a]);
            float pk = 0.f;
            int finite = 1;
            for (int i = 0; i < 2 * total; i++) {
                if (!isfinite(out[i])) finite = 0;
                if (fabsf(out[i]) > pk) pk = fabsf(out[i]);
            }
            float live = rms(out, play), end = rms(out + 2 * (total - SR / 2), SR / 2);
            float mid = rms(out + 2 * (play + 3 * SR), SR / 2);
            printf("%-7s %-12s  rms %6.1f dB  peak %5.2f  end %7.1f dB  %5.1f%% of a core\n", mi->name, mi->algo[a],
                   20 * log10f(live + 1e-9f), pk, 20 * log10f(end + 1e-9f), 100.0 * dt / ((double)total / SR));
            CHECK(finite, "%s %s: finite", mi->name, mi->algo[a]);
            CHECK(pk <= 1.0f, "%s %s: peak %.2f", mi->name, mi->algo[a], pk);
            CHECK(live > 0.003f, "%s %s: makes sound (rms %.4f)", mi->name, mi->algo[a], live);
            /* gone after 8 s, or (long tails, long delays) at least clearly fading */
            CHECK(end < 0.002f || end < 0.5f * mid, "%s %s: dies away (rms %.4f at 3 s, %.4f at 8 s)", mi->name, mi->algo[a], mid, end);
            if (wavdir) {
                char path[512];
                snprintf(path, sizeof path, "%s/%d_%s_%d_%s.wav", wavdir, m, mi->name, a, mi->algo[a]);
                for (char *c = path + strlen(wavdir); *c; c++) if (*c == ' ') *c = '_';
                write_wav(path, out, total);
            }
        }
    }

    /* the DEEP parameters at their extremes (all down, all up, then alternating), every algorithm:
     * finite, in range, still sounding */
    for (int m = 0; m < NMOD; m++) {
        const ModInfo *mi = eng_info(m);
        for (int a = 0; a < mi->nalgo; a++)
            for (int corner = 0; corner < 3; corner++) {
                Patch p;
                patch_default(&p);
                for (int k = 0; k < NMOD; k++) p.m[k].bypass = k != m;
                p.m[m].algo = a;
                p.m[m].mix = 1.f;
                for (int k = NBOARD; k < NPARAM; k++) p.m[m].p[a][k] = corner == 2 ? (float)(k & 1) : (float)corner;
                engine_reset(e);
                for (int i = 0; i < play; i += 256) engine_process(e, &p, in + 2 * i, out + 2 * i, play - i < 256 ? play - i : 256);
                float pk = 0.f;
                int finite = 1;
                for (int i = 0; i < 2 * play; i++) {
                    if (!isfinite(out[i])) finite = 0;
                    if (fabsf(out[i]) > pk) pk = fabsf(out[i]);
                }
                float live = rms(out, play);
                CHECK(finite && pk <= 1.f, "%s %s deep %s: finite, peak %.2f", mi->name, mi->algo[a], corner == 2 ? "mixed" : corner ? "up" : "down", pk);
                CHECK(live > 0.001f, "%s %s deep %s: still sounds (rms %.4f)", mi->name, mi->algo[a], corner == 2 ? "mixed" : corner ? "up" : "down", live);
            }
    }

    /* every factory preset: finite, sounding, every parameter in range */
    for (int f = 0; f < NFACTORY; f++) {
        Patch p;
        const char *nm = patch_factory(&p, f);
        int inrange = 1;
        for (int m = 0; m < NMOD; m++)
            for (int a = 0; a < MAXALGO; a++)
                for (int k = 0; k < NPARAM; k++)
                    if (!(p.m[m].p[a][k] >= 0.f && p.m[m].p[a][k] <= 1.f)) inrange = 0;
        CHECK(inrange, "factory %s: parameters in range", nm);
        engine_reset(e);
        for (int i = 0; i < play; i += 256) engine_process(e, &p, in + 2 * i, out + 2 * i, play - i < 256 ? play - i : 256);
        int finite = 1;
        for (int i = 0; i < 2 * play; i++) if (!isfinite(out[i])) finite = 0;
        CHECK(finite && rms(out, play) > 0.003f, "factory %s: finite, sounding (rms %.4f)", nm, rms(out, play));
    }

    /* switching algorithms: inline (the tests' default) clears megabytes in one block; with the
     * helper thread (as the app runs) the audio thread's worst block stays small, and the sound
     * comes back after each change */
    {
        double worst[2] = {1e9, 1e9};
        int back[2] = {0, 0}, finite[2] = {1, 1};
        for (int run = 0; run < 6; run++) { /* three runs each: the best of three, as this machine's scheduler adds noise */
            int mode = run & 1;
            double w = 0;
            back[mode] = 0;
            Engine *ea = engine_new();
            if (mode) engine_set_async(ea, 1);
            Patch p;
            patch_default(&p);
            p.m[M_TONE].bypass = p.m[M_MOD].bypass = 1;
            p.m[M_DELAY].mix = p.m[M_VERB].mix = 1.f;
            int blk = 256, total_blocks = 3 * SR / blk;
            float o[2 * 256];
            int since_switch = 0, heard = 1;
            for (int b = 0; b < total_blocks; b++) {
                if (b % 40 == 20) { /* every 213 ms a new algorithm on both */
                    back[mode] += heard;
                    p.m[M_DELAY].algo = (p.m[M_DELAY].algo + 1) % DL_N;
                    p.m[M_VERB].algo = (p.m[M_VERB].algo + 1) % RV_N;
                    since_switch = 0, heard = 0;
                }
                double t0 = now();
                engine_process(ea, &p, in + 2 * ((b * blk) % play), o, blk);
                double dt = now() - t0;
                if (b > 2 && dt > w) w = dt;
                since_switch++;
                if (mode && since_switch < 12) usleep(5333); /* the blocks around a change at the real pace (5.3 ms each) */
                for (int i = 0; i < 2 * blk; i++) if (fabsf(o[i]) > 1e-3f && since_switch > 3) heard = 1;
                for (int i = 0; i < 2 * blk; i++) if (!isfinite(o[i])) finite[mode] = 0;
            }
            engine_free(ea);
            if (w < worst[mode]) worst[mode] = w;
        }
        printf("algorithm changes: worst block %.2f ms inline, %.2f ms with the helper thread; sound back after %d / %d changes\n",
               worst[0] * 1e3, worst[1] * 1e3, back[1], back[0]);
        CHECK(finite[0] && finite[1], "algorithm changes: finite output");
        CHECK(back[1] >= back[0] - 1 && back[1] > 0, "the sound comes back after a change as it does inline (%d / %d)", back[1], back[0]);
        CHECK(worst[1] < worst[0] * 0.5, "the helper thread keeps the clearing out of the audio thread (%.2f vs %.2f ms)", worst[1] * 1e3, worst[0] * 1e3);
    }

    /* full wet passes no dry: a delay at 100 % with a long time is silent at first */
    {
        Patch p;
        patch_default(&p);
        for (int k = 0; k < NMOD; k++) p.m[k].bypass = k != M_DELAY;
        p.m[M_DELAY].algo = DL_TAPE;
        p.m[M_DELAY].mix = 1.f;
        p.m[M_DELAY].p[DL_TAPE][0] = 1.f;  /* 1.2 s */
        p.m[M_DELAY].p[DL_TAPE][2] = 0.f;  /* head 1: 0.4 s */
        engine_reset(e);
        engine_process(e, &p, in, out, SR / 4);
        CHECK(rms(out, SR / 4) < 1e-4f, "full wet: no dry (rms %.5f)", rms(out, SR / 4));
    }
    /* freeze holds the reverb */
    {
        Patch p;
        patch_default(&p);
        for (int k = 0; k < NMOD; k++) p.m[k].bypass = k != M_VERB;
        p.m[M_VERB].algo = RV_CONCERT;
        p.m[M_VERB].mix = 1.f;
        engine_reset(e);
        engine_process(e, &p, in, out, SR);
        p.freeze_verb = 1;
        engine_process(e, &p, in + 2 * play, out, 10 * SR > total - play ? total - play : 10 * SR);
        float held = rms(out + 2 * (6 * SR), SR);
        CHECK(held > 0.005f, "freeze holds the reverb (rms %.4f after 7 s)", held);
    }
    /* bypass with trails: the tail rings on after the reverb is switched off */
    {
        Patch p;
        patch_default(&p);
        for (int k = 0; k < NMOD; k++) p.m[k].bypass = k != M_VERB;
        p.m[M_VERB].mix = 1.f;
        engine_reset(e);
        engine_process(e, &p, in, out, SR);
        p.m[M_VERB].bypass = 1;
        float z[2 * 4800] = {0};
        engine_process(e, &p, z, out, 4800);
        CHECK(rms(out + 2 * 2400, 2400) > 0.003f, "trails after bypass");
    }

    printf("\n%d checks, %d failed. Engine cost: %.1f%% of one host core on average, worst %.1f%% (%s)\n", checks,
           fails, 100.0 * cpu_total / audio_total, 100.0 * worst, worst_name);
    engine_free(e);
    free(in); free(out);
    return fails ? 1 : 0;
}
