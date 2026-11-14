/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TrimFX: a four-module FX chain for the TrimUI Brick Pro (and Brick), for
 * an FX send: audio comes in from a USB interface, goes through
 *
 *     TONE -> MOD -> DELAY -> REVERB
 *
 * and back out, all wet if you like. The two analog sticks play the
 * effects as two X/Y pads: in FOCUS mode they move the four PERFORM
 * parameters of the module on screen, in MATRIX mode each stick axis moves
 * up to two parameters anywhere in the chain. See README.md.
 *
 * Settings (env, in launch.sh): TFX_BUFFER, TFX_AUDIO=<output name part>,
 * TFX_INPUT=<capture name part>, TFX_LEDS=0, TFX_LOG=0|1|2, TFX_WINDOWED=1,
 * TFX_SCREEN_WAKE (set by mux_launch.sh), TFX_SHOT=<file.bmp> (desktop: run
 * a scripted tour and save screenshots, then quit). */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "brick_input.h"
#include "dsp.h"
#include "engine.h"
#include "gfx.h"
#include "perflog.h"
#include "ringleds.h"

#ifndef TFX_VERSION
#define TFX_VERSION "0.1.0"
#endif
#define W SCREEN_W
#define H SCREEN_H

static int env_int(const char *n, int def)
{
    const char *v = getenv(n);
    return v && *v ? atoi(v) : def;
}

/* ================================================================ the patch, shared with the audio thread */
/* the UI owns `base` (what is stored) and builds `live` (base + the sticks) each frame, then
 * publishes it under a sequence lock; the audio thread copies it at the start of each buffer */
static Patch base, live;
static Patch pub;
static atomic_uint pub_seq;
static char patch_name[48] = "INIT";

static void publish(const Patch *p)
{
    atomic_fetch_add_explicit(&pub_seq, 1, memory_order_acq_rel); /* odd: writing */
    memcpy(&pub, p, sizeof pub);
    atomic_fetch_add_explicit(&pub_seq, 1, memory_order_acq_rel);
}

static int take(Patch *into)
{
    unsigned a = atomic_load_explicit(&pub_seq, memory_order_acquire);
    if (a & 1) return 0;
    memcpy(into, &pub, sizeof *into);
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&pub_seq, memory_order_relaxed) == a;
}

/* ================================================================ sound */
static Engine *eng;
static SDL_AudioDeviceID out_dev, in_dev;
static int rate = 48000, buffer = 512;
static char out_name[128], in_name[128];   /* what is open ("" = default) */
static int out_pick = -1, in_pick = -1;    /* SETUP: -1 auto (USB first), else a device index */
static char out_want[128], in_want[128];   /* the names picked, kept across hot-plugs */

#define RING 16384 /* frames, a power of two */
static float ring[RING * 2];
static atomic_uint ring_w, ring_r;
static atomic_uint underruns, overruns;
static atomic_int have_input;

static void flush_denormals(void)
{
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= 1u << 24;
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__SSE__)
    __builtin_ia32_ldmxcsr(__builtin_ia32_stmxcsr() | 0x8040);
#endif
}

static void in_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    const int16_t *s = (const int16_t *)stream;
    int frames = len / 4;
    unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed), r = atomic_load_explicit(&ring_r, memory_order_acquire);
    for (int i = 0; i < frames; i++) {
        if (w - r >= RING - 1) { atomic_fetch_add(&overruns, 1); break; }
        ring[2 * (w & (RING - 1))] = s[2 * i] * (1.f / 32768.f);
        ring[2 * (w & (RING - 1)) + 1] = s[2 * i + 1] * (1.f / 32768.f);
        w++;
    }
    atomic_store_explicit(&ring_w, w, memory_order_release);
}

static void out_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    static int ftz;
    static Patch p;
    static int have_p;
    static float in[4096 * 2], out[4096 * 2];
    if (!ftz) flush_denormals(), ftz = 1;
    int16_t *s = (int16_t *)stream;
    int frames = len / 4;
    if (frames > 4096) frames = 4096;
    uint64_t t0 = perf_audio_begin(frames);
    if (take(&p)) have_p = 1;
    /* the input: what the capture side has given; keep about a buffer and a half in hand */
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire), r = atomic_load_explicit(&ring_r, memory_order_relaxed);
    unsigned fill = w - r, keep = (unsigned)buffer * 3 / 2 + (unsigned)frames;
    if (fill > keep + (unsigned)buffer * 2) { r = w - keep; atomic_fetch_add(&overruns, 1); fill = w - r; } /* drifted ahead: drop */
    int got = atomic_load(&have_input) ? (int)(fill < (unsigned)frames ? fill : (unsigned)frames) : 0;
    if (atomic_load(&have_input) && got < frames && fill > 0) atomic_fetch_add(&underruns, 1);
    for (int i = 0; i < got; i++) {
        in[2 * i] = ring[2 * (r & (RING - 1))];
        in[2 * i + 1] = ring[2 * (r & (RING - 1)) + 1];
        r++;
    }
    for (int i = got; i < frames; i++) in[2 * i] = in[2 * i + 1] = 0.f;
    atomic_store_explicit(&ring_r, r, memory_order_release);
    if (have_p) engine_process(eng, &p, in, out, frames);
    else memset(out, 0, sizeof(float) * 2 * frames);
    for (int i = 0; i < 2 * frames; i++) {
        float v = out[i] * 32767.f;
        s[i] = (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
    }
    for (int i = 2 * frames; i < len / 2; i++) s[i] = 0;
    ringleds_audio(s, frames);
    perf_audio_output(s, frames);
    perf_audio_end(t0);
}

static int is_usb_name(const char *n) { return n && (strcasestr(n, "usb") || strcasestr(n, "uac") || strcasestr(n, "dac")); }

/* the device to open: a picked one, else what TFX_AUDIO / TFX_INPUT names, else a USB one, else the default */
static const char *pick(int capture)
{
    const char *want = capture ? in_want : out_want;
    if (*want) {
        for (int i = 0; i < SDL_GetNumAudioDevices(capture); i++) {
            const char *n = SDL_GetAudioDeviceName(i, capture);
            if (n && !strcmp(n, want)) return n;
        }
    }
    const char *env = getenv(capture ? "TFX_INPUT" : "TFX_AUDIO");
    if (env && !strcasecmp(env, "internal")) return NULL;
    for (int i = 0; i < SDL_GetNumAudioDevices(capture); i++) {
        const char *n = SDL_GetAudioDeviceName(i, capture);
        if (n && (env && *env && strcasecmp(env, "auto") ? strcasestr(n, env) != NULL : is_usb_name(n))) return n;
    }
    return NULL;
}

static void audio_open_out(const char *name)
{
    char keep[128];
    snprintf(keep, sizeof keep, "%s", name ? name : "");
    name = *keep ? keep : NULL;
    if (out_dev) SDL_CloseAudioDevice(out_dev), out_dev = 0;
    SDL_AudioSpec want = {0}, have;
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)buffer;
    want.callback = out_cb;
    out_dev = SDL_OpenAudioDevice(name, 0, &want, &have, 0);
    if (!out_dev && name) name = NULL, out_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!out_dev) { perf_log("AUDIO", "no output: %s", SDL_GetError()); return; }
    snprintf(out_name, sizeof out_name, "%s", name ? name : "");
    perf_audio_open(rate, have.samples, name);
    perf_log("AUDIO", "out: SDL %s, %d Hz, %d frames, %s", SDL_GetCurrentAudioDriver(), have.freq, have.samples,
             name ? name : "default");
    SDL_PauseAudioDevice(out_dev, 0);
}

static void audio_open_in(const char *name)
{
    char keep[128];
    snprintf(keep, sizeof keep, "%s", name ? name : "");
    name = *keep ? keep : NULL;
    if (in_dev) SDL_CloseAudioDevice(in_dev), in_dev = 0;
    atomic_store(&have_input, 0);
    SDL_AudioSpec want = {0}, have;
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)buffer;
    want.callback = in_cb;
    in_dev = SDL_OpenAudioDevice(name, 1, &want, &have, 0);
    if (!in_dev && name) name = NULL, in_dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (!in_dev) { in_name[0] = 0; perf_log("AUDIO", "no capture: %s", SDL_GetError()); return; }
    snprintf(in_name, sizeof in_name, "%s", name ? name : "");
    perf_log("AUDIO", "in: %d Hz, %d frames, %s", have.freq, have.samples, name ? name : "default");
    atomic_store(&ring_r, atomic_load(&ring_w));
    atomic_store(&have_input, 1);
    SDL_PauseAudioDevice(in_dev, 0);
}

static void audio_follow(int force)
{
    const char *o = pick(0);
    if (force || !out_dev || strcmp(o ? o : "", out_name)) audio_open_out(o);
    const char *i = pick(1);
    if (force || !in_dev || strcmp(i ? i : "", in_name)) audio_open_in(i);
}

/* ================================================================ presets and state */
static char app_dir[512] = ".";
static int preset_slot = 0; /* 0..15 */

/* the sticks, part of a preset */
enum { AX_LX, AX_LY, AX_RX, AX_RY, NAX };
typedef struct { int mod, param; float depth; } Target; /* param -1 = mix; mod -1 = none */
static Target matrix[NAX][2];
static int stick_matrix = 0; /* 0 FOCUS, 1 MATRIX */
static int stick_latch = 0;  /* 0 SPRING (offsets), 1 LATCH (drag the setting) */

static void matrix_default(void)
{
    static const Target D[NAX][2] = {
        {{M_MOD, 0, 1.f}, {M_TONE, 0, 0.5f}},
        {{M_DELAY, 1, 0.8f}, {M_MOD, 1, 0.6f}},
        {{M_DELAY, 0, 1.f}, {M_VERB, 3, 0.6f}},
        {{M_VERB, 0, 1.f}, {M_VERB, -1, 0.5f}},
    };
    memcpy(matrix, D, sizeof matrix);
}

static void save_patch(const char *path, const Patch *p)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# TrimFX %s\nname=%s\n", TFX_VERSION, patch_name);
    fprintf(f, "in_db=%g\nout_db=%g\ndry=%g\nbpm=%g\norder=%d\n", p->in_db, p->out_db, p->dry, p->bpm, p->order);
    for (int m = 0; m < NMOD; m++) {
        fprintf(f, "m%d.algo=%d\nm%d.bypass=%d\nm%d.mix=%g\n", m, p->m[m].algo, m, p->m[m].bypass, m, p->m[m].mix);
        for (int a = 0; a < eng_info(m)->nalgo; a++)
            for (int k = 0; k < NPARAM; k++) fprintf(f, "m%d.p%d.%d=%g\n", m, a, k, p->m[m].p[a][k]);
    }
    fprintf(f, "sticks.matrix=%d\nsticks.latch=%d\n", stick_matrix, stick_latch);
    for (int a = 0; a < NAX; a++)
        for (int s = 0; s < 2; s++)
            fprintf(f, "x%d.%d=%d,%d,%g\n", a, s, matrix[a][s].mod, matrix[a][s].param, matrix[a][s].depth);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, path);
}

static int load_patch(const char *path, Patch *p)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    patch_default(p);
    matrix_default();
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        int m, a, i, s;
        if (!strcmp(k, "name")) snprintf(patch_name, sizeof patch_name, "%s", v);
        else if (!strcmp(k, "in_db")) p->in_db = strtof(v, NULL);
        else if (!strcmp(k, "out_db")) p->out_db = strtof(v, NULL);
        else if (!strcmp(k, "dry")) p->dry = strtof(v, NULL);
        else if (!strcmp(k, "bpm")) p->bpm = strtof(v, NULL);
        else if (!strcmp(k, "order")) p->order = atoi(v) % NORDER;
        else if (!strcmp(k, "sticks.matrix")) stick_matrix = atoi(v) != 0;
        else if (!strcmp(k, "sticks.latch")) stick_latch = atoi(v) != 0;
        else if (sscanf(k, "m%d.p%d.%d", &m, &a, &i) == 3) {
            if (m >= 0 && m < NMOD && a >= 0 && a < MAXALGO && i >= 0 && i < NPARAM) p->m[m].p[a][i] = clampf(strtof(v, NULL), 0.f, 1.f);
        } else if (sscanf(k, "m%d.%*s", &m) == 1 && m >= 0 && m < NMOD) {
            if (strstr(k, ".algo")) p->m[m].algo = atoi(v) % eng_info(m)->nalgo;
            else if (strstr(k, ".bypass")) p->m[m].bypass = atoi(v) != 0;
            else if (strstr(k, ".mix")) p->m[m].mix = clampf(strtof(v, NULL), 0.f, 1.f);
        } else if (sscanf(k, "x%d.%d", &a, &s) == 2 && a >= 0 && a < NAX && s >= 0 && s < 2) {
            Target t = {-1, 0, 0.f};
            if (sscanf(v, "%d,%d,%f", &t.mod, &t.param, &t.depth) == 3 && t.mod >= -1 && t.mod < NMOD && t.param >= -1 && t.param < NPARAM)
                matrix[a][s] = t;
        }
    }
    fclose(f);
    if (p->bpm < 40.f || p->bpm > 240.f) p->bpm = 100.f;
    return 0;
}

static void slot_path(char *out, int n, int slot) { snprintf(out, n, "%s/presets/slot%02d.txt", app_dir, slot + 1); }
static void state_path(char *out, int n) { snprintf(out, n, "%s/state.txt", app_dir); }

/* ================================================================ the UI */
enum { PG_TONE, PG_MOD, PG_DELAY, PG_VERB, PG_STICKS, PG_SETUP, NPAGE };
static const char *const PAGE_N[NPAGE] = {"TONE", "MOD", "DELAY", "REVERB", "STICKS", "SETUP"};
static int page = PG_MOD, focus_mod = M_MOD;
static int row[NPAGE];
static int scroll[NPAGE];
static char toast_msg[96];
static Uint32 toast_until;
static float sx[2], sy[2];     /* the sticks: x right, y up */
static float hold_v[NAX];      /* HOLD: the stick values caught */
static int hold_on;
static float tap_t[4];
static int taps;
static Uint32 last_change, saved_at;
static int dirty;
static EngMeters meters;
static float vu_in[2], vu_out[2], vu_mod[NMOD];
static int factory_pick = 0;
static int is_pro;

static void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_msg, sizeof toast_msg, fmt, ap);
    va_end(ap);
    toast_until = SDL_GetTicks() + 1800;
}

static void changed(void) { dirty = 1; last_change = SDL_GetTicks(); }

/* the stick axes now (HOLD keeps them where they were caught) */
static void axes(float a[NAX])
{
    float v[NAX] = {sx[0], sy[0], sx[1], sy[1]};
    for (int k = 0; k < NAX; k++) a[k] = hold_on ? hold_v[k] : v[k];
}

/* the parameters a stick axis moves, in FOCUS or MATRIX mode */
static int axis_targets(int ax, Target out[2])
{
    if (!stick_matrix) {
        out[0] = (Target){focus_mod, ax, 1.f};
        return 1;
    }
    int n = 0;
    for (int s = 0; s < 2; s++)
        if (matrix[ax][s].mod >= 0) out[n++] = matrix[ax][s];
    return n;
}

static float *target_ptr(Patch *p, Target t)
{
    if (t.mod < 0 || t.mod >= NMOD) return NULL;
    if (t.param < 0) return &p->m[t.mod].mix;
    return &p->m[t.mod].p[p->m[t.mod].algo][t.param];
}

/* live = base, moved by the sticks (SPRING: an offset while held; LATCH: the setting itself is dragged) */
static void build_live(float dt)
{
    float a[NAX];
    axes(a);
    if (stick_latch && !hold_on) {
        for (int ax = 0; ax < NAX; ax++) {
            if (fabsf(a[ax]) < 0.02f) continue;
            Target t[2];
            int n = axis_targets(ax, t);
            for (int k = 0; k < n; k++) {
                float *v = target_ptr(&base, t[k]);
                if (v) *v = clampf(*v + a[ax] * t[k].depth * dt * 0.6f, 0.f, 1.f);
            }
            changed();
        }
    }
    live = base;
    if (!stick_latch || hold_on) {
        for (int ax = 0; ax < NAX; ax++) {
            Target t[2];
            int n = axis_targets(ax, t);
            for (int k = 0; k < n; k++) {
                float *v = target_ptr(&live, t[k]);
                if (v) *v = clampf(*v + a[ax] * t[k].depth, 0.f, 1.f);
            }
        }
    }
}

/* ---------------------------------------------------------------- rows */
static int nrows(int pg)
{
    if (pg <= PG_VERB) return 2 + NPARAM;
    if (pg == PG_STICKS) return 3 + NAX * 2 * 2;
    return 12;
}

static int param_used(int m, int k)
{
    const char *n = eng_info(m)->pname[base.m[m].algo][k];
    return n && *n;
}

/* the targets a matrix slot can take: NONE, then per module MIX, P1..P8 */
#define NTARGET (1 + NMOD * (1 + NPARAM))
static Target target_of(int i)
{
    if (i <= 0) return (Target){-1, 0, 0.f};
    i--;
    return (Target){i / (1 + NPARAM), i % (1 + NPARAM) - 1, 0.f};
}
static int index_of(Target t)
{
    if (t.mod < 0) return 0;
    return 1 + t.mod * (1 + NPARAM) + t.param + 1;
}
static void target_name(Target t, char *o, int n)
{
    if (t.mod < 0) { snprintf(o, n, "NONE"); return; }
    const ModInfo *mi = eng_info(t.mod);
    if (t.param < 0) snprintf(o, n, "%s MIX", mi->name);
    else {
        const char *pn = mi->pname[base.m[t.mod].algo][t.param];
        snprintf(o, n, "%s %s", mi->name, pn && *pn ? pn : "-");
    }
}

static void setup_row_text(int r, char *label, char *val, int n)
{
    switch (r) {
    case 0: snprintf(label, n, "INPUT"); snprintf(val, n, "%s", in_dev ? (*in_name ? in_name : "DEFAULT") : "NONE"); break;
    case 1: snprintf(label, n, "OUTPUT"); snprintf(val, n, "%s", *out_name ? out_name : "DEFAULT"); break;
    case 2: snprintf(label, n, "INPUT GAIN"); snprintf(val, n, "%+.1f dB", base.in_db); break;
    case 3: snprintf(label, n, "OUTPUT GAIN"); snprintf(val, n, "%+.1f dB", base.out_db); break;
    case 4: snprintf(label, n, "DRY IN RETURN"); snprintf(val, n, base.dry < 0.005f ? "OFF (SEND)" : "%.0f %%", base.dry * 100.f); break;
    case 5: snprintf(label, n, "TEMPO"); snprintf(val, n, "%.1f BPM", base.bpm); break;
    case 6: snprintf(label, n, "ORDER"); snprintf(val, n, "TONE>%s", eng_order_name(base.order)); break;
    case 7: snprintf(label, n, "PRESET SLOT"); snprintf(val, n, "%d", preset_slot + 1); break;
    case 8: snprintf(label, n, "SAVE TO SLOT"); snprintf(val, n, "A / START"); break;
    case 9: snprintf(label, n, "LOAD SLOT"); snprintf(val, n, "A"); break;
    case 10: {
        Patch tmp;
        snprintf(label, n, "FACTORY");
        snprintf(val, n, "%d %s  (A)", factory_pick + 1, patch_factory(&tmp, factory_pick));
    } break;
    default: snprintf(label, n, "INIT ALL"); snprintf(val, n, "A, twice"); break;
    }
}

static void cycle_device(int capture, int dir)
{
    int n = SDL_GetNumAudioDevices(capture);
    int *pk = capture ? &in_pick : &out_pick;
    *pk += dir;
    if (*pk < -1) *pk = n - 1;
    if (*pk >= n) *pk = -1;
    char *want = capture ? in_want : out_want;
    if (*pk < 0) want[0] = 0;
    else snprintf(want, 128, "%s", SDL_GetAudioDeviceName(*pk, capture));
    if (capture) audio_open_in(pick(1)); else audio_open_out(pick(0));
    toast("%s: %s", capture ? "INPUT" : "OUTPUT", *pk < 0 ? "AUTO" : want);
}

static void load_slot(int slot)
{
    char path[600];
    slot_path(path, sizeof path, slot);
    Patch p;
    if (load_patch(path, &p) == 0) { base = p; toast("LOADED SLOT %d: %s", slot + 1, patch_name); changed(); }
    else toast("SLOT %d IS EMPTY", slot + 1);
}

static void save_slot(int slot)
{
    char path[600];
    snprintf(path, sizeof path, "%s/presets", app_dir);
    mkdir(path, 0755);
    slot_path(path, sizeof path, slot);
    if (!strcmp(patch_name, "INIT") || !*patch_name) snprintf(patch_name, sizeof patch_name, "SLOT %d", slot + 1);
    save_patch(path, &base);
    toast("SAVED TO SLOT %d", slot + 1);
}

static void adjust(int dir, int big)
{
    float step = big ? 0.05f : 0.01f;
    if (page <= PG_VERB) {
        int m = page;
        ModState *ms = &base.m[m];
        int r = row[page];
        if (r == 0) {
            int na = eng_info(m)->nalgo;
            ms->algo = (ms->algo + dir + na) % na;
        } else if (r == 1)
            ms->mix = clampf(ms->mix + dir * step, 0.f, 1.f);
        else {
            int k = r - 2;
            if (!param_used(m, k)) return;
            ms->p[ms->algo][k] = clampf(ms->p[ms->algo][k] + dir * step, 0.f, 1.f);
        }
    } else if (page == PG_STICKS) {
        int r = row[page];
        if (r == 0) stick_matrix ^= 1;
        else if (r == 1) stick_latch ^= 1;
        else if (r == 2) { matrix_default(); toast("MATRIX RESET"); }
        else {
            int q = r - 3, ax = q / 4, s = (q / 2) % 2, isdepth = q & 1;
            Target *t = &matrix[ax][s];
            if (isdepth) t->depth = clampf(t->depth + dir * (big ? 0.1f : 0.05f), -1.f, 1.f);
            else {
                float d = t->depth;
                Target nt = target_of((index_of(*t) + dir + NTARGET) % NTARGET);
                nt.depth = d == 0.f ? 1.f : d;
                *t = nt;
            }
        }
    } else {
        switch (row[page]) {
        case 0: cycle_device(1, dir); break;
        case 1: cycle_device(0, dir); break;
        case 2: base.in_db = clampf(base.in_db + dir * (big ? 3.f : 0.5f), -24.f, 24.f); break;
        case 3: base.out_db = clampf(base.out_db + dir * (big ? 3.f : 0.5f), -24.f, 24.f); break;
        case 4: base.dry = clampf(base.dry + dir * step, 0.f, 1.f); break;
        case 5: base.bpm = clampf(roundf(base.bpm + dir * (big ? 5.f : 1.f)), 40.f, 240.f); break;
        case 6: base.order = (base.order + dir + NORDER) % NORDER; break;
        case 7: preset_slot = (preset_slot + dir + 16) % 16; break;
        case 10: factory_pick = (factory_pick + dir + NFACTORY) % NFACTORY; break;
        default: break;
        }
    }
    changed();
}

static int init_armed;
static void press_a(void)
{
    if (page <= PG_VERB) {
        base.m[page].bypass ^= 1;
        toast("%s %s", eng_info(page)->name, base.m[page].bypass ? "BYPASSED" : "ON");
        changed();
        return;
    }
    if (page == PG_SETUP) {
        switch (row[page]) {
        case 8: save_slot(preset_slot); break;
        case 9: load_slot(preset_slot); break;
        case 10: {
            const char *nm = patch_factory(&base, factory_pick);
            snprintf(patch_name, sizeof patch_name, "%s", nm);
            matrix_default();
            toast("FACTORY: %s", nm);
            changed();
        } break;
        case 11:
            if (init_armed) { patch_default(&base); matrix_default(); snprintf(patch_name, sizeof patch_name, "INIT"); toast("ALL BACK TO INIT"); init_armed = 0; changed(); }
            else { init_armed = 1; toast("PRESS A AGAIN TO RESET EVERYTHING"); }
            break;
        }
    }
}

static void tap_tempo(void)
{
    float t = SDL_GetTicks() / 1000.f;
    if (taps > 0 && t - tap_t[(taps - 1) & 3] > 2.f) taps = 0;
    tap_t[taps & 3] = t;
    taps++;
    if (taps >= 2) {
        int n = taps > 4 ? 4 : taps;
        float first = tap_t[(taps - n) & 3], last = tap_t[(taps - 1) & 3];
        float bpm = 60.f * (n - 1) / (last - first);
        if (bpm >= 40.f && bpm <= 240.f) { base.bpm = roundf(bpm * 10.f) / 10.f; changed(); }
    }
    toast("TAP  %.1f BPM", base.bpm);
}

static void load_factory_or_slot(int dir)
{
    preset_slot = (preset_slot + dir + 16) % 16;
    char path[600];
    slot_path(path, sizeof path, preset_slot);
    if (access(path, R_OK) == 0) load_slot(preset_slot);
    else {
        const char *nm = patch_factory(&base, preset_slot % NFACTORY);
        snprintf(patch_name, sizeof patch_name, "%s", nm);
        matrix_default();
        toast("SLOT %d (FACTORY): %s", preset_slot + 1, nm);
        changed();
    }
}

/* ---------------------------------------------------------------- input */
static int held[BI_COUNT];
static Uint32 rep_at[BI_COUNT];
static int rep_n[BI_COUNT];
static int running = 1;

static void button(BiButton b, int down)
{
    held[b] = down;
    if (!down) return;
    rep_at[b] = SDL_GetTicks() + 350;
    rep_n[b] = 0;
    switch (b) {
    case BI_UP: row[page] = (row[page] - 1 + nrows(page)) % nrows(page); break;
    case BI_DOWN: row[page] = (row[page] + 1) % nrows(page); break;
    case BI_LEFT: adjust(-1, 0); break;
    case BI_RIGHT: adjust(+1, 0); break;
    case BI_L1: page = (page - 1 + NPAGE) % NPAGE; if (page <= PG_VERB) focus_mod = page; break;
    case BI_R1: page = (page + 1) % NPAGE; if (page <= PG_VERB) focus_mod = page; break;
    case BI_L2: case BI_R2: {
        int m = focus_mod, na = eng_info(m)->nalgo;
        base.m[m].algo = (base.m[m].algo + (b == BI_R2 ? 1 : -1) + na) % na;
        toast("%s: %s", eng_info(m)->name, eng_info(m)->algo[base.m[m].algo]);
        changed();
    } break;
    case BI_A: press_a(); break;
    case BI_B: stick_matrix ^= 1; toast("STICKS: %s", stick_matrix ? "MATRIX" : "FOCUS"); changed(); break;
    case BI_X: stick_latch ^= 1; toast("STICKS: %s", stick_latch ? "LATCH" : "SPRING"); changed(); break;
    case BI_Y:
        hold_on ^= 1;
        if (hold_on) { float v[NAX] = {sx[0], sy[0], sx[1], sy[1]}; memcpy(hold_v, v, sizeof v); }
        toast("STICKS %s", hold_on ? "HELD" : "LIVE");
        break;
    case BI_SELECT: tap_tempo(); break;
    case BI_START: save_slot(preset_slot); break;
    case BI_L3: base.freeze_dly ^= 1; toast("DELAY %s", base.freeze_dly ? "FROZEN" : "FLOWING"); break;
    case BI_R3: base.freeze_verb ^= 1; toast("REVERB %s", base.freeze_verb ? "FROZEN" : "FLOWING"); break;
    case BI_F1: load_factory_or_slot(-1); break;
    case BI_F2: load_factory_or_slot(+1); break;
    case BI_MENU: running = 0; break;
    default: break;
    }
    if (b != BI_UP && b != BI_DOWN && b != BI_LEFT && b != BI_RIGHT) init_armed = init_armed && b == BI_A;
}

static void repeats(void)
{
    Uint32 now = SDL_GetTicks();
    static const BiButton R[4] = {BI_UP, BI_DOWN, BI_LEFT, BI_RIGHT};
    for (int i = 0; i < 4; i++) {
        BiButton b = R[i];
        if (!held[b] || now < rep_at[b]) continue;
        rep_n[b]++;
        rep_at[b] = now + (rep_n[b] > 12 ? 30 : 70);
        if (b == BI_UP) row[page] = (row[page] - 1 + nrows(page)) % nrows(page);
        else if (b == BI_DOWN) row[page] = (row[page] + 1) % nrows(page);
        else if (!(page == PG_SETUP && row[page] <= 1)) adjust(b == BI_LEFT ? -1 : 1, rep_n[b] > 12);
    }
}

/* desktop: the keyboard as the device's buttons and sticks */
static void keyboard(const SDL_Event *e)
{
    if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP) return;
    if (e->key.repeat) return;
    int down = e->type == SDL_KEYDOWN;
    BiButton b = BI_NONE;
    switch (e->key.keysym.sym) {
    case SDLK_UP: b = BI_UP; break;
    case SDLK_DOWN: b = BI_DOWN; break;
    case SDLK_LEFT: b = BI_LEFT; break;
    case SDLK_RIGHT: b = BI_RIGHT; break;
    case SDLK_z: b = BI_A; break;
    case SDLK_x: b = BI_B; break;
    case SDLK_a: b = BI_X; break;
    case SDLK_s: b = BI_Y; break;
    case SDLK_q: b = BI_L1; break;
    case SDLK_e: b = BI_R1; break;
    case SDLK_1: b = BI_L2; break;
    case SDLK_3: b = BI_R2; break;
    case SDLK_TAB: b = BI_SELECT; break;
    case SDLK_RETURN: b = BI_START; break;
    case SDLK_ESCAPE: b = BI_MENU; break;
    case SDLK_f: b = BI_L3; break;
    case SDLK_g: b = BI_R3; break;
    case SDLK_5: b = BI_F1; break;
    case SDLK_6: b = BI_F2; break;
    /* i j k l: the left stick, numpad 8 4 2 6 / t f g h: the right */
    case SDLK_i: sy[0] = down ? 1.f : 0.f; return;
    case SDLK_k: sy[0] = down ? -1.f : 0.f; return;
    case SDLK_j: sx[0] = down ? -1.f : 0.f; return;
    case SDLK_l: sx[0] = down ? 1.f : 0.f; return;
    case SDLK_KP_8: sy[1] = down ? 1.f : 0.f; return;
    case SDLK_KP_2: sy[1] = down ? -1.f : 0.f; return;
    case SDLK_KP_4: sx[1] = down ? -1.f : 0.f; return;
    case SDLK_KP_6: sx[1] = down ? 1.f : 0.f; return;
    default: return;
    }
    button(b, down);
}

/* ================================================================ drawing */
#define C_BG 0x0b0d14
#define C_PANEL 0x161a26
#define C_PANEL2 0x1e2333
#define C_LINE 0x2c3348
#define C_TEXT 0xdfe3ee
#define C_DIM 0x7d8599
#define C_ACC 0x8a7dff   /* violet */
#define C_WARM 0xffb45a
#define C_COOL 0x5ad1ff
#define C_GOOD 0x6ee7a0
#define C_BAD 0xff5a6e
static const uint32_t MODCOL[NMOD] = {0xffb45a, 0xc77dff, 0x5ad1ff, 0x6ee7a0};
static const uint32_t AXCOL[NAX] = {0xff7ab6, 0xffd15a, 0x5ad1ff, 0x8affc1};
static const char *const AXN[NAX] = {"LX", "LY", "RX", "RY"};

static void text_c(int cx, int y, const char *s, uint32_t c, int font) { gfx_text(cx - gfx_text_w(s, font) / 2, y, s, c, font); }
static void text_r(int rx, int y, const char *s, uint32_t c, int font) { gfx_text(rx - gfx_text_w(s, font), y, s, c, font); }

/* fit a string into w pixels of the small font */
static void fit(char *s, int w)
{
    int max = w / 9;
    if ((int)strlen(s) > max && max > 2) s[max - 1] = '.', s[max] = 0;
}

static void meter_v(int x, int y, int w, int h, float lin, uint32_t c)
{
    gfx_fill(x, y, w, h, 0x0e1119);
    float db = lin2db(lin + 1e-9f), t = clampf((db + 48.f) / 48.f, 0.f, 1.f);
    int fh = (int)(t * h);
    gfx_fill(x, y + h - fh, w, fh, db > -1.f ? C_BAD : db > -9.f ? C_WARM : c);
}

/* an X/Y pad: the stored setting as a ring, where the sticks have it as a dot, a line between */
static void xy_pad(int x, int y, int sz, float bx, float by, float lx, float ly, uint32_t col, const char *xl, const char *yl, int big)
{
    gfx_fill(x, y, sz, sz, 0x0e1119);
    gfx_rect(x, y, sz, sz, 1, C_LINE);
    for (int g = 1; g < 4; g++) {
        gfx_fill(x + sz * g / 4, y + 1, 1, sz - 2, 0x171b28);
        gfx_fill(x + 1, y + sz * g / 4, sz - 2, 1, 0x171b28);
    }
    float px = x + 6 + bx * (sz - 12), py = y + sz - 6 - by * (sz - 12);
    float qx = x + 6 + lx * (sz - 12), qy = y + sz - 6 - ly * (sz - 12);
    /* crosshair at the live position */
    gfx_fill_a((int)qx, y + 1, 1, sz - 2, col, 70);
    gfx_fill_a(x + 1, (int)qy, sz - 2, 1, col, 70);
    if (fabsf(px - qx) > 1.f || fabsf(py - qy) > 1.f) gfx_line(px, py, qx, qy, big ? 2.f : 1.2f, gfx_mix(C_LINE, col, 0.6f));
    gfx_ring(px, py, big ? 9.f : 5.f, big ? 2.f : 1.5f, gfx_mix(C_DIM, col, 0.3f));
    gfx_disc(qx, qy, big ? 8.f : 4.f, col);
    if (big) {
        gfx_text(x + 6, y + sz + 4, xl, C_DIM, FONT_SMALL);
        char yb[40];
        snprintf(yb, sizeof yb, "%s", yl);
        text_r(x + sz - 4, y + sz + 4, yb, C_DIM, FONT_SMALL);
    }
}

static void draw_top(void)
{
    gfx_fill(0, 0, W, 52, C_PANEL);
    gfx_text(16, 8, "TRIMFX", C_ACC, FONT_LARGE);
    int x = 150;
    for (int p = 0; p < NPAGE; p++) {
        int w = gfx_text_w(PAGE_N[p], FONT_SMALL) + 22;
        uint32_t c = p <= PG_VERB ? MODCOL[p] : C_TEXT;
        if (p == page) gfx_fill(x, 8, w, 36, gfx_mix(C_PANEL, c, 0.25f)), gfx_fill(x, 40, w, 4, c);
        gfx_text(x + 11, 15, PAGE_N[p], p == page ? C_TEXT : C_DIM, FONT_SMALL);
        x += w + 4;
    }
    char b[96];
    snprintf(b, sizeof b, "%d %s", preset_slot + 1, patch_name);
    fit(b, 220);
    text_r(W - 120, 6, b, C_TEXT, FONT_SMALL);
    snprintf(b, sizeof b, "%.0f BPM  %s%s", base.bpm, stick_matrix ? "MATRIX" : "FOCUS", stick_latch ? "/LATCH" : "/SPRING");
    text_r(W - 120, 27, b, C_DIM, FONT_SMALL);
    meter_v(W - 100, 8, 8, 36, vu_in[0], C_COOL);
    meter_v(W - 90, 8, 8, 36, vu_in[1], C_COOL);
    meter_v(W - 30, 8, 8, 36, vu_out[0], C_GOOD);
    meter_v(W - 20, 8, 8, 36, vu_out[1], C_GOOD);
    gfx_text(W - 78, 8, "IN", C_DIM, FONT_SMALL);
    gfx_text(W - 78, 26, "OUT", C_DIM, FONT_SMALL);
}

/* the chain: each module's box with its algorithm, mix, and a small X/Y of where its sticks are */
static void draw_chain(void)
{
    int ord[4] = {M_TONE, 0, 0, 0};
    eng_order(base.order, ord + 1);
    int x = 16, y = 60, bw = 236, bh = 100;
    for (int k = 0; k < 4; k++) {
        int m = ord[k];
        const ModInfo *mi = eng_info(m);
        int on = !base.m[m].bypass, sel = focus_mod == m;
        uint32_t c = MODCOL[m];
        gfx_fill(x, y, bw, bh, sel ? C_PANEL2 : C_PANEL);
        gfx_fill(x, y, 4, bh, on ? c : C_LINE);
        if (sel) gfx_rect(x, y, bw, bh, 2, gfx_mix(C_PANEL, c, 0.7f));
        gfx_text(x + 12, y + 6, mi->name, on ? c : C_DIM, FONT_SMALL);
        char b[64];
        snprintf(b, sizeof b, "%s", mi->algo[base.m[m].algo]);
        fit(b, 140);
        gfx_text(x + 12, y + 30, b, on ? C_TEXT : C_DIM, FONT_SMALL);
        snprintf(b, sizeof b, on ? "MIX %3.0f%%" : "BYPASS", live.m[m].mix * 100.f);
        gfx_text(x + 12, y + 54, b, on ? C_DIM : C_BAD, FONT_SMALL);
        if ((m == M_DELAY && base.freeze_dly) || (m == M_VERB && base.freeze_verb)) gfx_text(x + 12, y + 76, "FROZEN", C_COOL, FONT_SMALL);
        else {
            /* the level out of this module */
            float t = clampf((lin2db(vu_mod[m] + 1e-9f) + 48.f) / 48.f, 0.f, 1.f);
            gfx_fill(x + 12, y + 84, 130, 5, 0x0e1119);
            gfx_fill(x + 12, y + 84, (int)(130 * t), 5, on ? c : C_LINE);
        }
        /* two mini X/Y pads: (P1, P2) and (P3, P4) */
        int a = base.m[m].algo;
        const float *bp = base.m[m].p[a], *lp = live.m[m].p[a];
        xy_pad(x + bw - 84, y + 8, 40, bp[0], bp[1], lp[0], lp[1], c, "", "", 0);
        xy_pad(x + bw - 42, y + 8, 40, bp[2], bp[3], lp[2], lp[3], c, "", "", 0);
        if (k < 3) gfx_tri(x + bw + 2, y + bh / 2 - 6, x + bw + 2, y + bh / 2 + 6, x + bw + 9, y + bh / 2, C_DIM);
        x += bw + 12;
    }
}

static void draw_row(int y, int sel, const char *label, const char *val, float bar, uint32_t col, uint32_t ax_col, const char *axn,
                     float live_bar)
{
    gfx_fill(16, y, 584, 34, sel ? C_PANEL2 : C_PANEL);
    if (sel) gfx_fill(16, y, 4, 34, col);
    gfx_text(30, y + 6, label, sel ? C_TEXT : C_DIM, FONT_SMALL);
    if (axn) {
        gfx_fill(176, y + 7, 30, 20, gfx_mix(C_PANEL, ax_col, 0.35f));
        text_c(191, y + 6, axn, ax_col, FONT_SMALL);
    }
    if (bar >= 0.f) {
        int bx = 216, bwid = 200;
        gfx_fill(bx, y + 14, bwid, 6, 0x0e1119);
        gfx_fill(bx, y + 14, (int)(bwid * bar), 6, gfx_mix(C_LINE, col, 0.55f));
        if (live_bar >= 0.f && fabsf(live_bar - bar) > 0.004f) {
            gfx_fill(bx + (int)(bwid * live_bar) - 2, y + 9, 4, 16, ax_col);
        }
        gfx_fill(bx + (int)(bwid * bar) - 1, y + 10, 2, 14, col);
    }
    char v[64];
    snprintf(v, sizeof v, "%s", val);
    fit(v, 170);
    text_r(592, y + 6, v, sel ? C_TEXT : C_DIM, FONT_SMALL);
}

static void draw_module_page(int m)
{
    const ModInfo *mi = eng_info(m);
    const ModState *ms = &base.m[m];
    int a = ms->algo, y0 = 172;
    uint32_t col = MODCOL[m];
    char v[64];
    snprintf(v, sizeof v, "%d/%d  %s", a + 1, mi->nalgo, mi->algo[a]);
    draw_row(y0, row[page] == 0, "ALGORITHM", v, -1.f, col, 0, NULL, -1.f);
    snprintf(v, sizeof v, ms->mix >= 0.999f ? "100%% WET" : "%.0f %%", live.m[m].mix * 100.f);
    draw_row(y0 + 38, row[page] == 1, "MIX", v, ms->mix, col, C_TEXT, NULL, live.m[m].mix);
    for (int k = 0; k < NPARAM; k++) {
        const char *pn = mi->pname[a][k];
        int y = y0 + 76 + k * 38 + (k >= 4 ? 8 : 0);
        if (!pn || !*pn) {
            draw_row(y, row[page] == 2 + k, "-", "", -1.f, col, 0, NULL, -1.f);
            continue;
        }
        float lv = live.m[m].p[a][k];
        mi->fmt(a, k, lv, &live, v, sizeof v);
        /* which stick axis moves it (FOCUS mode: the first four are the PERFORM set) */
        const char *axn = NULL;
        uint32_t axc = 0;
        if (!stick_matrix && k < 4) axn = AXN[k], axc = AXCOL[k];
        else if (stick_matrix)
            for (int ax = 0; ax < NAX && !axn; ax++)
                for (int s = 0; s < 2; s++)
                    if (matrix[ax][s].mod == m && matrix[ax][s].param == k) { axn = AXN[ax]; axc = AXCOL[ax]; break; }
        draw_row(y, row[page] == 2 + k, pn, v, ms->p[a][k], col, axc ? axc : col, axn, lv);
    }
    gfx_text(30, y0 + 76 + 4 * 38 - 4, "", C_DIM, FONT_SMALL);
    /* the right side: what it is, and the two X/Y pads */
    int rx = 616;
    gfx_fill(rx, y0, 392, 72, C_PANEL);
    gfx_text(rx + 12, y0 + 6, mi->algo[a], col, FONT_LARGE);
    char bl[64];
    snprintf(bl, sizeof bl, "%s", mi->blurb[a]);
    fit(bl, 372);
    gfx_text(rx + 12, y0 + 44, bl, C_DIM, FONT_SMALL);
    const float *bp = ms->p[a], *lp = live.m[m].p[a];
    char xl[40], yl[40];
    int py = y0 + 92, sz = 186;
    for (int s = 0; s < 2; s++) {
        int px = rx + s * (sz + 20);
        snprintf(xl, sizeof xl, "%s>%s", s ? "RX" : "LX", mi->pname[a][2 * s]);
        snprintf(yl, sizeof yl, "%s^%s", s ? "RY" : "LY", mi->pname[a][2 * s + 1]);
        fit(xl, sz / 2 + 10);
        fit(yl, sz / 2 + 10);
        gfx_text(px, py - 22, s ? "RIGHT STICK" : "LEFT STICK", stick_matrix ? C_DIM : AXCOL[2 * s], FONT_SMALL);
        xy_pad(px, py, sz, bp[2 * s], bp[2 * s + 1], lp[2 * s], lp[2 * s + 1], col, xl, yl, 1);
    }
    if (stick_matrix) gfx_text(rx, py + sz + 30, "MATRIX: sticks follow the STICKS page", C_WARM, FONT_SMALL);
    gfx_text(rx, py + sz + 54, "A bypass  L2/R2 algorithm  B focus/matrix", C_DIM, FONT_SMALL);
    gfx_text(rx, py + sz + 76, "X spring/latch  Y hold  L3/R3 freeze", C_DIM, FONT_SMALL);
    gfx_text(rx, py + sz + 98, "SELECT tap  START save  F1/F2 presets", C_DIM, FONT_SMALL);
}

static void draw_sticks_page(void)
{
    int y0 = 172, vis = 15;
    int r = row[page];
    if (r < scroll[page]) scroll[page] = r;
    if (r >= scroll[page] + vis) scroll[page] = r - vis + 1;
    for (int i = scroll[page]; i < nrows(page) && i < scroll[page] + vis; i++) {
        int y = y0 + (i - scroll[page]) * 38;
        char lab[48], val[64];
        uint32_t ac = C_ACC;
        const char *axn = NULL;
        float bar = -1.f;
        if (i == 0) snprintf(lab, sizeof lab, "MODE"), snprintf(val, sizeof val, stick_matrix ? "MATRIX" : "FOCUS (PAGE)");
        else if (i == 1) snprintf(lab, sizeof lab, "BEHAVIOUR"), snprintf(val, sizeof val, stick_latch ? "LATCH (DRAG)" : "SPRING (OFFSET)");
        else if (i == 2) snprintf(lab, sizeof lab, "RESET MATRIX"), snprintf(val, sizeof val, "< >");
        else {
            int q = i - 3, ax = q / 4, s = (q / 2) % 2;
            Target *t = &matrix[ax][s];
            ac = AXCOL[ax];
            axn = AXN[ax];
            if (q & 1) {
                snprintf(lab, sizeof lab, "  DEPTH %d", s + 1);
                snprintf(val, sizeof val, "%+.0f %%", t->depth * 100.f);
                bar = 0.5f + 0.5f * t->depth;
            } else {
                snprintf(lab, sizeof lab, "TARGET %d", s + 1);
                target_name(*t, val, sizeof val);
            }
        }
        draw_row(y, i == r, lab, val, bar, ac, ac, axn, -1.f);
    }
    /* the sticks themselves */
    int rx = 640, sz = 160;
    float a[NAX];
    axes(a);
    for (int s = 0; s < 2; s++) {
        int py = y0 + 10 + s * (sz + 70);
        gfx_text(rx, py - 2, s ? "RIGHT STICK" : "LEFT STICK", C_TEXT, FONT_SMALL);
        xy_pad(rx, py + 22, sz, 0.5f, 0.5f, 0.5f + 0.5f * a[2 * s], 0.5f + 0.5f * a[2 * s + 1], AXCOL[2 * s], AXN[2 * s], AXN[2 * s + 1], 1);
        for (int k = 0; k < 2; k++) {
            Target t[2];
            int n = axis_targets(2 * s + k, t);
            int ty = py + 26 + k * 70;
            gfx_text(rx + sz + 14, ty, AXN[2 * s + k], AXCOL[2 * s + k], FONT_SMALL);
            for (int j = 0; j < n; j++) {
                char nm[48];
                target_name(t[j], nm, sizeof nm);
                fit(nm, 180);
                gfx_text(rx + sz + 14, ty + 20 + j * 20, nm, C_DIM, FONT_SMALL);
            }
        }
    }
    if (hold_on) gfx_text(rx, y0 + 2 * (sz + 70) + 10, "HELD (Y)", C_WARM, FONT_SMALL);
}

static void draw_setup_page(void)
{
    int y0 = 172;
    for (int i = 0; i < nrows(page); i++) {
        char lab[48], val[160];
        setup_row_text(i, lab, val, sizeof lab < sizeof val ? 48 : 48);
        draw_row(y0 + i * 38, i == row[page], lab, val, -1.f, C_ACC, 0, NULL, -1.f);
    }
    int rx = 616;
    gfx_fill(rx, y0, 392, 300, C_PANEL);
    gfx_text(rx + 12, y0 + 8, "FX SEND", C_ACC, FONT_LARGE);
    const char *lines[] = {
        "Plug a USB interface into the top port:",
        "its input is the send, its output the",
        "return. DRY IN RETURN off = all wet.",
        "Each module's MIX goes to 100 % wet.",
        "",
        "SETUP < > on INPUT / OUTPUT picks a",
        "device (AUTO: the first USB one).",
        "",
        "DELAY and REVERB bypass with trails.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof *lines; i++) gfx_text(rx + 12, y0 + 52 + i * 24, lines[i], C_DIM, FONT_SMALL);
    char b[128];
    snprintf(b, sizeof b, "underruns %u  overruns %u", atomic_load(&underruns), atomic_load(&overruns));
    gfx_text(rx + 12, y0 + 316, b, C_DIM, FONT_SMALL);
    snprintf(b, sizeof b, "TrimFX %s  %s  buf %d", TFX_VERSION, is_pro ? "Brick Pro" : "Brick", buffer);
    gfx_text(rx + 12, y0 + 340, b, C_DIM, FONT_SMALL);
}

static void draw(void)
{
    gfx_clear(C_BG);
    draw_top();
    draw_chain();
    if (page <= PG_VERB) draw_module_page(page);
    else if (page == PG_STICKS) draw_sticks_page();
    else draw_setup_page();
    if (!in_dev) gfx_text(16, H - 26, "NO INPUT DEVICE: plug in a USB interface", C_BAD, FONT_SMALL);
    if (SDL_GetTicks() < toast_until) {
        int w = gfx_text_w(toast_msg, FONT_LARGE) + 40;
        gfx_fill_a(W / 2 - w / 2, H - 92, w, 54, 0x000000, 200);
        gfx_rect(W / 2 - w / 2, H - 92, w, 54, 2, C_ACC);
        text_c(W / 2, H - 83, toast_msg, C_TEXT, FONT_LARGE);
    }
}

/* ================================================================ the screen */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_Surface *surf;
static int screen_reset, draw_fails;
static Uint32 last_rebuild;

static void make_screen(int renderer)
{
    if (tex) SDL_DestroyTexture(tex), tex = NULL;
    if (renderer) {
        if (ren) SDL_DestroyRenderer(ren);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H) : NULL;
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    last_rebuild = SDL_GetTicks();
}

static void screen_wake(void)
{
    const char *cmd = getenv("TFX_SCREEN_WAKE");
    if (!cmd || !*cmd) return;
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            execl("/bin/sh", "sh", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static void present(void)
{
    static int frames;
    for (int y = 0; y < H; y++) {
        Uint32 *row_ = (Uint32 *)((Uint8 *)surf->pixels + y * surf->pitch);
        for (int x = 0; x < W; x++) row_[x] |= 0xFF000000u; /* opaque: MustardOS shows alpha */
    }
    if (screen_reset) make_screen(screen_reset == 2), screen_reset = 0;
    int bad = !tex || SDL_UpdateTexture(tex, NULL, surf->pixels, surf->pitch) < 0;
    if (ren) {
        SDL_RenderClear(ren);
        bad |= !tex || SDL_RenderCopy(ren, tex, NULL, NULL) < 0;
        SDL_RenderPresent(ren);
    } else
        bad = 1;
    if (bad && SDL_GetTicks() - last_rebuild > 1000) make_screen(++draw_fails > 2 || !ren);
    if (++frames == 10) screen_wake();
    if (frames == 10 + 75 && getenv("TFX_SCREEN_WAKE")) make_screen(1);
}

/* ================================================================ the scripted tour (desktop screenshots) */
static void shot(const char *base_path, const char *tag)
{
    char p[600];
    snprintf(p, sizeof p, "%s_%s.bmp", base_path, tag);
    SDL_SaveBMP(surf, p);
}

static void tour(const char *path)
{
    Patch keep = base;
    patch_factory(&base, 2);
    snprintf(patch_name, sizeof patch_name, "PS RITUAL");
    sx[0] = 0.55f; sy[0] = 0.35f; sx[1] = -0.4f; sy[1] = 0.6f;
    static const int pages[] = {PG_MOD, PG_TONE, PG_DELAY, PG_VERB, PG_STICKS, PG_SETUP};
    for (unsigned i = 0; i < sizeof pages / sizeof *pages; i++) {
        page = pages[i];
        if (page <= PG_VERB) focus_mod = page;
        row[page] = page <= PG_VERB ? 2 : 3;
        build_live(0.f);
        vu_in[0] = vu_in[1] = 0.3f; vu_out[0] = 0.5f; vu_out[1] = 0.45f;
        for (int m = 0; m < NMOD; m++) vu_mod[m] = 0.2f + 0.1f * m;
        draw();
        shot(path, PAGE_N[page]);
    }
    base = keep;
}

/* ================================================================ main */
int main(int argc, char **argv)
{
    (void)argc;
    /* the app's folder: where the binary is (presets and state live there) */
    {
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char *s = strrchr(exe, '/');
            if (s) *s = 0, snprintf(app_dir, sizeof app_dir, "%s", exe);
        } else if (argv[0] && strrchr(argv[0], '/')) {
            snprintf(app_dir, sizeof app_dir, "%s", argv[0]);
            *strrchr(app_dir, '/') = 0;
        }
        const char *d = getenv("TFX_DIR");
        if (d && *d) snprintf(app_dir, sizeof app_dir, "%s", d);
    }
    perf_init("TFX", "TrimFX " TFX_VERSION);
    if (env_int("TFX_MLOCK", 1) && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) perf_log("START", "could not lock memory");
    buffer = env_int("TFX_BUFFER", 512);
    if (buffer < 64 || buffer > 4096) buffer = 512;
    const char *shot_path = getenv("TFX_SHOT");

    eng = engine_new();
    if (!eng) { fprintf(stderr, "out of memory\n"); return 1; }
    patch_default(&base);
    matrix_default();
    {
        char sp[600];
        state_path(sp, sizeof sp);
        Patch p;
        if (!shot_path && load_patch(sp, &p) == 0) base = p;
        else snprintf(patch_name, sizeof patch_name, "%s", patch_factory(&base, 0));
    }
    build_live(0.f);
    publish(&live);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    win = SDL_CreateWindow("TrimFX", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                           SDL_WINDOW_SHOWN | (env_int("TFX_WINDOWED", 0) || shot_path ? 0 : SDL_WINDOW_FULLSCREEN));
    surf = SDL_CreateRGBSurface(0, W, H, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (!win || !surf) { fprintf(stderr, "screen: %s\n", SDL_GetError()); return 1; }
    gfx_target((uint32_t *)surf->pixels);
    make_screen(1);
    if (shot_path) {
        tour(shot_path);
        engine_free(eng);
        SDL_Quit();
        return 0;
    }

    bi_init("TFX");
    bi_open_joysticks();
    is_pro = bi_is_brickpro();
    int leds = env_int("TFX_LEDS", 1) ? ringleds_open("TFX", is_pro) : 0;
    perf_log("START", "TrimFX %s on %s, %s, LEDs %s", TFX_VERSION, is_pro ? "Brick Pro" : "Brick",
             bi_is_mustardos() ? "MustardOS" : "TrimUI OS", leds ? "on" : "off");
    if (!is_pro) toast("NO STICKS HERE: USE THE STICKS PAGE / A GAMEPAD");

    audio_follow(1);

    Uint32 last = SDL_GetTicks();
    while (running) {
        Uint32 t0 = SDL_GetTicks();
        float dt = (t0 - last) / 1000.f;
        last = t0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_AUDIODEVICEADDED || e.type == SDL_AUDIODEVICEREMOVED) audio_follow(0);
            else if (e.type == SDL_JOYDEVICEADDED) bi_open_joysticks();
            else if (e.type == SDL_RENDER_TARGETS_RESET) screen_reset = screen_reset > 1 ? screen_reset : 1;
            else if (e.type == SDL_RENDER_DEVICE_RESET) screen_reset = 2;
            keyboard(&e);
            BiEvent ev[8];
            int n = bi_event(&e, ev, 8);
            for (int i = 0; i < n; i++) {
                if (ev[i].type == BI_EV_STICK) { sx[ev[i].stick] = ev[i].x; sy[ev[i].stick] = ev[i].y; continue; }
                if (perf_level() >= PERF_DETAIL) perf_log("INPUT", "%s %s", bi_button_names[ev[i].button], ev[i].down ? "down" : "up");
                button(ev[i].button, ev[i].down);
            }
        }
        repeats();
        build_live(dt);
        publish(&live);

        engine_meters(eng, &meters);
        for (int c = 0; c < 2; c++) {
            vu_in[c] = fmaxf(meters.in_pk[c], vu_in[c] * 0.85f);
            vu_out[c] = fmaxf(meters.out_pk[c], vu_out[c] * 0.85f);
        }
        for (int m = 0; m < NMOD; m++) vu_mod[m] = fmaxf(meters.mod_pk[m], vu_mod[m] * 0.85f);

        /* the state, a couple of seconds after the last change */
        if (dirty && t0 - last_change > 2000 && t0 - saved_at > 2000) {
            char sp[600];
            state_path(sp, sizeof sp);
            save_patch(sp, &base);
            dirty = 0;
            saved_at = t0;
        }

        if (leds) {
            uint32_t f1 = base.freeze_dly ? MODCOL[M_DELAY] : 0, f2 = base.freeze_verb ? MODCOL[M_VERB] : 0;
            int step = (int)(t0 / (60000.f / base.bpm / 4.f));
            ringleds_update(sx[0], sy[0], MODCOL[focus_mod], sx[1], sy[1], AXCOL[2], 1, step, base.bpm, f1, f2);
        }

        draw();
        present();
        perf_frame((uint64_t)(SDL_GetTicks() - t0) * 1000);
        perf_tick();
        Uint32 spent = SDL_GetTicks() - t0;
        if (spent < 16) SDL_Delay(16 - spent);
    }

    {
        char sp[600];
        state_path(sp, sizeof sp);
        save_patch(sp, &base);
    }
    if (out_dev) SDL_CloseAudioDevice(out_dev);
    if (in_dev) SDL_CloseAudioDevice(in_dev);
    engine_free(eng);
    ringleds_close();
    bi_switch_close();
    perf_close();
    SDL_Quit();
    return 0;
}
