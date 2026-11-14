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

static SDL_Surface *surf;

/* ================================================================ the UI: one pane, a pedalboard */
/* Four pedals side by side (TONE, MOD, DELAY, REVERB) and a MASTER strip. Each pedal: an LCD
 * (the algorithm and the control being set), four PERFORM knobs (on the sticks), four DEEP
 * sliders, a MIX fader and a footswitch. Below: the selected pedal's two X/Y pads. Rarely used
 * things (devices, save / load, the stick matrix) are in overlays opened from MASTER. */
enum { COL_TONE, COL_MOD, COL_DELAY, COL_VERB, COL_MASTER, NCOL };
enum { R_ALGO, R_K1, R_K2, R_K3, R_K4, R_D1, R_D2, R_D3, R_D4, R_MIX, NPROW };
enum { MR_IN, MR_OUT, MR_DRY, MR_TEMPO, MR_ORDER, MR_SLOT, MR_SETUP, MR_STICKS, NMROW };
enum { OV_NONE, OV_SETUP, OV_STICKS };
enum { SR_INPUT, SR_OUTPUT, SR_SAVE, SR_LOAD, SR_FACTORY, SR_INIT, NSROW };
#define NXROW (3 + NAX * 2 * 2)
static int col = COL_MOD, focus_mod = M_MOD;
static int sel[NCOL] = {R_K1, R_K1, R_K1, R_K1, MR_IN};
static int overlay, ov_sel[3], ov_scroll[3];
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

static int rows_now(void)
{
    if (overlay == OV_SETUP) return NSROW;
    if (overlay == OV_STICKS) return NXROW;
    return col == COL_MASTER ? NMROW : NPROW;
}
static int *sel_now(void) { return overlay ? &ov_sel[overlay] : &sel[col]; }

static void set_col(int c)
{
    col = (c + NCOL) % NCOL;
    if (col < COL_MASTER) focus_mod = col;
}

/* ---------------------------------------------------------------- turning things */
static int init_armed;

static void adjust(int dir, int big)
{
    float step = big ? 0.05f : 0.01f;
    if (overlay == OV_SETUP) {
        switch (ov_sel[OV_SETUP]) {
        case SR_INPUT: cycle_device(1, dir); break;
        case SR_OUTPUT: cycle_device(0, dir); break;
        case SR_SAVE: case SR_LOAD: preset_slot = (preset_slot + dir + 16) % 16; break;
        case SR_FACTORY: factory_pick = (factory_pick + dir + NFACTORY) % NFACTORY; break;
        default: break;
        }
        changed();
        return;
    }
    if (overlay == OV_STICKS) {
        int r = ov_sel[OV_STICKS];
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
        changed();
        return;
    }
    if (col == COL_MASTER) {
        switch (sel[col]) {
        case MR_IN: base.in_db = clampf(base.in_db + dir * (big ? 3.f : 0.5f), -24.f, 24.f); break;
        case MR_OUT: base.out_db = clampf(base.out_db + dir * (big ? 3.f : 0.5f), -24.f, 24.f); break;
        case MR_DRY: base.dry = clampf(base.dry + dir * step, 0.f, 1.f); break;
        case MR_TEMPO: base.bpm = clampf(roundf(base.bpm + dir * (big ? 5.f : 1.f)), 40.f, 240.f); break;
        case MR_ORDER: base.order = (base.order + dir + NORDER) % NORDER; break;
        case MR_SLOT: load_factory_or_slot(dir); return;
        default: return;
        }
        changed();
        return;
    }
    int m = col;
    ModState *ms = &base.m[m];
    int r = sel[col];
    if (r == R_ALGO) {
        int na = eng_info(m)->nalgo;
        ms->algo = (ms->algo + dir + na) % na;
    } else if (r == R_MIX)
        ms->mix = clampf(ms->mix + dir * step, 0.f, 1.f);
    else {
        int k = r - R_K1;
        if (!param_used(m, k)) return;
        ms->p[ms->algo][k] = clampf(ms->p[ms->algo][k] + dir * step, 0.f, 1.f);
    }
    changed();
}

static void press_a(void)
{
    if (overlay == OV_SETUP) {
        switch (ov_sel[OV_SETUP]) {
        case SR_SAVE: save_slot(preset_slot); break;
        case SR_LOAD: load_slot(preset_slot); break;
        case SR_FACTORY: {
            const char *nm = patch_factory(&base, factory_pick);
            snprintf(patch_name, sizeof patch_name, "%s", nm);
            matrix_default();
            toast("FACTORY: %s", nm);
            changed();
        } break;
        case SR_INIT:
            if (init_armed) { patch_default(&base); matrix_default(); snprintf(patch_name, sizeof patch_name, "INIT"); toast("ALL BACK TO INIT"); init_armed = 0; changed(); return; }
            init_armed = 1;
            toast("PRESS A AGAIN TO RESET EVERYTHING");
            return;
        }
        return;
    }
    if (overlay == OV_STICKS) { adjust(1, 0); return; }
    if (col == COL_MASTER) {
        if (sel[col] == MR_SETUP) overlay = OV_SETUP;
        else if (sel[col] == MR_STICKS) overlay = OV_STICKS;
        else if (sel[col] == MR_SLOT) save_slot(preset_slot);
        return;
    }
    /* the footswitch */
    base.m[col].bypass ^= 1;
    toast("%s %s", eng_info(col)->name, base.m[col].bypass ? "BYPASSED" : "ON");
    changed();
}

/* ---------------------------------------------------------------- input */
static int held[BI_COUNT];
static Uint32 rep_at[BI_COUNT];
static int rep_n[BI_COUNT];
static int running = 1;

static void move_row(int dir)
{
    int *s = sel_now(), n = rows_now();
    *s = (*s + dir + n) % n;
}

static void button(BiButton b, int down)
{
    held[b] = down;
    if (!down) return;
    rep_at[b] = SDL_GetTicks() + 350;
    rep_n[b] = 0;
    if (b != BI_A) init_armed = 0;
    switch (b) {
    case BI_UP: move_row(-1); break;
    case BI_DOWN: move_row(+1); break;
    case BI_LEFT: adjust(-1, 0); break;
    case BI_RIGHT: adjust(+1, 0); break;
    case BI_L1: case BI_R1:
        if (overlay) overlay = overlay == OV_SETUP ? OV_STICKS : OV_SETUP;
        else set_col(col + (b == BI_R1 ? 1 : -1));
        break;
    case BI_L2: case BI_R2: {
        int m = focus_mod, na = eng_info(m)->nalgo;
        base.m[m].algo = (base.m[m].algo + (b == BI_R2 ? 1 : -1) + na) % na;
        toast("%s: %s", eng_info(m)->name, eng_info(m)->algo[base.m[m].algo]);
        changed();
    } break;
    case BI_A: press_a(); break;
    case BI_B:
        if (overlay) overlay = OV_NONE;
        else { stick_matrix ^= 1; toast("STICKS: %s", stick_matrix ? "MATRIX" : "FOCUS"); changed(); }
        break;
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
    case BI_HOME: overlay = overlay ? OV_NONE : OV_SETUP; break;
    case BI_MENU: running = 0; break;
    default: break;
    }
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
        if (b == BI_UP || b == BI_DOWN) move_row(b == BI_UP ? -1 : 1);
        else if (!(overlay == OV_SETUP && ov_sel[OV_SETUP] <= SR_OUTPUT) && !(col == COL_MASTER && !overlay && sel[col] == MR_SLOT))
            adjust(b == BI_LEFT ? -1 : 1, rep_n[b] > 12);
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
    case SDLK_h: b = BI_HOME; break;
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
#define C_BG 0x1c1d20
#define C_PANEL 0x15171f
#define C_PANEL2 0x1d2030
#define C_LINE 0x2c3044
#define C_TEXT 0xe4e7f0
#define C_DIM 0x7c8398
#define C_ACC 0x8a7dff
#define C_WARM 0xffb45a
#define C_COOL 0x5ad1ff
#define C_GOOD 0x6ee7a0
#define C_BAD 0xff5a6e
#define C_LCD 0x0c1a14
#define C_LCDTXT 0x9df2c4
static const uint32_t MODCOL[NMOD] = {0xe8961e, 0x6a4fd0, 0x2fb04a, 0x1f78d6}; /* the name bands */
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

/* an X/Y pad: the stored setting as a ring, where the sticks have it as a dot, a line between */
static void xy_pad(int x, int y, int sz, float bx, float by, float lx, float ly, uint32_t col_, const char *xl, const char *yl, int big)
{
    gfx_fill(x, y, sz, sz, 0x0e1119);
    gfx_rect(x, y, sz, sz, 1, C_LINE);
    for (int g = 1; g < 4; g++) {
        gfx_fill(x + sz * g / 4, y + 1, 1, sz - 2, 0x171b28);
        gfx_fill(x + 1, y + sz * g / 4, sz - 2, 1, 0x171b28);
    }
    float px = x + 6 + bx * (sz - 12), py = y + sz - 6 - by * (sz - 12);
    float qx = x + 6 + lx * (sz - 12), qy = y + sz - 6 - ly * (sz - 12);
    gfx_fill_a((int)qx, y + 1, 1, sz - 2, col_, 70);
    gfx_fill_a(x + 1, (int)qy, sz - 2, 1, col_, 70);
    if (fabsf(px - qx) > 1.f || fabsf(py - qy) > 1.f) gfx_line(px, py, qx, qy, big ? 2.f : 1.2f, gfx_mix(C_LINE, col_, 0.6f));
    gfx_ring(px, py, big ? 8.f : 5.f, big ? 2.f : 1.5f, gfx_mix(C_DIM, col_, 0.3f));
    gfx_disc(qx, qy, big ? 7.f : 4.f, col_);
    if (big && xl && *xl) {
        gfx_text(x + 4, y + sz + 2, xl, C_DIM, FONT_SMALL);
        text_r(x + sz - 2, y - 22, yl, C_DIM, FONT_SMALL);
    }
}

/* ---------------------------------------------------------------- knobs and sliders */
/* ---------------------------------------------------------------- the board's look: raw-aluminium
 * boxes with hand-stamped lettering, black knurled knobs with a pointer nub, chrome footswitches,
 * a Dymo-tape readout, and two joystick gimbals on the master box for the sticks. */
#define ALU 0xc4c6c7         /* sandblasted aluminium */
#define INK 0x26272a         /* stamped letters */
#define INK_LIGHT 0xe9eaeb   /* the stamp's lit edge */
#define ACCENT 0xff9a1f      /* what is selected */
#define BOARD 0x15171d
#define KNOB_A0 (0.75f * TFX_PI)   /* 7:30 */
#define KNOB_SPAN (1.5f * TFX_PI)  /* to 4:30 */

#define PED_W 196
#define PED_Y 36
#define PED_H 724
#define PED_X(k) (10 + (k) * (PED_W + 10))
#define MST_X PED_X(4)
#define MST_W (W - 10 - MST_X)

static void knob_pt(float cx, float cy, float r, float v, float *x, float *y)
{
    float a = KNOB_A0 + KNOB_SPAN * v;
    *x = cx + r * cosf(a);
    *y = cy + r * sinf(a);
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

/* the board and the five boxes, made once: speckled metal, rounded corners, lit top edges */
static uint32_t *bg;
static void make_bg(void)
{
    bg = malloc(sizeof(uint32_t) * W * H);
    if (!bg) return;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            /* a melting orange / pink backdrop, after the photo the boxes sit on */
            float zig = fabsf(fmodf(y * 0.021f + 40.f, 2.f) - 1.f) * 38.f;           /* zig-zag */
            float t = (x + zig + 9.f * sinf(y * 0.013f + x * 0.004f)) * 0.012f;
            uint32_t h = hash32((uint32_t)(y * W + x) * 2654435761u);
            t += ((h & 255) / 255.f - 0.5f) * 0.05f + (((h >> 8) % 7) == 0 ? 0.04f : 0.f); /* the smear */
            float f = t - floorf(t);
            bg[y * W + x] = f < 0.42f ? 0xffa516 : f < 0.62f ? 0xff8a0e : f < 0.86f ? 0xf27272 : 0xff9c14;
        }
    for (int k = 0; k < 5; k++) {
        int x0 = PED_X(k), w = k < 4 ? PED_W : MST_W, y0 = PED_Y, h = PED_H, rr = 12;
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++) {
                /* the rounded corners */
                int dx = x < x0 + rr ? x0 + rr - x : x >= x0 + w - rr ? x - (x0 + w - rr - 1) : 0;
                int dy = y < y0 + rr ? y0 + rr - y : y >= y0 + h - rr ? y - (y0 + h - rr - 1) : 0;
                if (dx * dx + dy * dy > rr * rr) continue;
                uint32_t hsh = hash32((uint32_t)(y * 7919 + x * 104729 + k));
                int v = 0xbe + (int)(hsh % 22) - 11;          /* the blast's speckle */
                if ((hsh >> 8) % 97 == 0) v -= 38;              /* a pit here and there */
                v += (int)(10.f * (1.f - (float)(y - y0) / h)); /* a little lighter at the top */
                int edge = x - x0 < 2 || y - y0 < 2 ? 22 : (x0 + w - x <= 3 || y0 + h - y <= 3) ? -45 : 0;
                v = v + edge;
                v = v < 0 ? 0 : v > 255 ? 255 : v;
                bg[y * W + x] = (uint32_t)(v << 16 | v << 8 | (v + 2 > 255 ? 255 : v + 2));
            }
        /* its shadow on the board */
        for (int y = y0 + h; y < y0 + h + 4 && y < H; y++)
            for (int x = x0 + 4; x < x0 + w + 2 && x < W; x++) bg[y * W + x] = 0x0a0b0e;
    }
}

/* hand-stamped letters: each one a little off line, pressed in (dark, with a lit lower edge) */
static int stamp(int x, int y, const char *s, int big, int spacing)
{
    int font = big ? FONT_LARGE : FONT_SMALL, cw = (big ? 16 : 9) + spacing;
    char ch[2] = {0, 0};
    uint32_t seed = 0;
    for (const char *p = s; *p; p++) seed = seed * 31 + (uint8_t)*p;
    for (int i = 0; s[i]; i++) {
        if (s[i] == ' ') continue;
        uint32_t h = hash32(seed + (uint32_t)i * 977u);
        int dy = (int)(h % 4) - 1, dx = (int)((h >> 4) % 3) - 1;
        ch[0] = s[i];
        gfx_text(x + i * cw + dx + 1, y + dy + 1, ch, INK_LIGHT, font);
        gfx_text(x + i * cw + dx, y + dy, ch, (h >> 9) % 5 ? INK : 0x45474b, font);
    }
    return (int)strlen(s) * cw;
}
static int stamp_w(const char *s, int big, int spacing) { return (int)strlen(s) * ((big ? 16 : 9) + spacing); }
static void stamp_c(int cx, int y, const char *s, int big, int spacing) { stamp(cx - stamp_w(s, big, spacing) / 2, y, s, big, spacing); }

/* a black knurled knob with a pointer nub; the stored value as a small punched dot on the metal,
 * the stick's colour beside the nub while a stick moves it */
static void knob(int cx, int cy, int r, float stored, float lv, uint32_t axc, int used, int selected)
{
    if (selected) gfx_ring(cx, cy, r + 15, 2.5f, ACCENT);
    if (used) {
        float dx_, dy_;
        knob_pt(cx, cy, r + 10, stored, &dx_, &dy_);
        gfx_disc(dx_ + 1, dy_ + 1, 2.2f, INK_LIGHT);
        gfx_disc(dx_, dy_, 2.2f, INK);
    }
    gfx_disc(cx + 2, cy + 3, r + 1, 0x7d7f82); /* its shadow on the metal */
    /* the pointer nub */
    float a = KNOB_A0 + KNOB_SPAN * (used ? lv : 0.5f);
    float tip[6] = {cx + (r + 9) * cosf(a), cy + (r + 9) * sinf(a), cx + r * cosf(a - 0.3f), cy + r * sinf(a - 0.3f),
                    cx + r * cosf(a + 0.3f), cy + r * sinf(a + 0.3f)};
    gfx_tri(tip[0], tip[1], tip[2], tip[3], tip[4], tip[5], 0x1b1c1f);
    gfx_disc(cx, cy, r, 0x1b1c1f);
    for (int i = 0; i < 48; i++) { /* the knurling */
        float t = i * TFX_2PI / 48.f;
        gfx_disc(cx + (r - 1.5f) * cosf(t), cy + (r - 1.5f) * sinf(t), 1.f, 0x3c3e43);
    }
    gfx_disc(cx, cy, r - 4, 0x26272b);
    gfx_disc(cx, cy, r - 7, 0x1e1f23);
    /* the gloss: a soft sheen and a bright streak, as on a lacquered cap */
    gfx_disc(cx - r * 0.2f, cy - r * 0.28f, r * 0.42f, 0x2b2c31);
    gfx_line(cx - r * 0.55f, cy - r * 0.12f, cx + r * 0.05f, cy - r * 0.42f, r > 20 ? 3.f : 2.f, 0x8d9096);
    gfx_line(cx + r * 0.1f, cy + r * 0.45f, cx + r * 0.45f, cy + r * 0.3f, 1.5f, 0x4a4c52);
    if (!used) return;
    float px = cx + (r - 3) * cosf(a), py = cy + (r - 3) * sinf(a);
    gfx_line(cx + r * 0.35f * cosf(a), cy + r * 0.35f * sinf(a), px, py, r > 20 ? 3.f : 2.f, 0xf4f4f4);
    if (fabsf(lv - stored) > 0.004f) gfx_disc(cx + (r + 13) * cosf(a), cy + (r + 13) * sinf(a), 3.5f, axc);
}

/* the LED in its black bezel */
static void led(int cx, int cy, uint32_t c, int lit)
{
    gfx_disc(cx, cy, 11.f, 0x101114);
    gfx_ring(cx, cy, 11.f, 2.f, 0x3a3c40);
    gfx_disc(cx, cy, 6.f, lit ? c : gfx_mix(0x101114, c, 0.18f));
    if (lit) { gfx_ring(cx, cy, 8.f, 2.f, gfx_mix(0x101114, c, 0.55f)); gfx_disc(cx - 2, cy - 2, 2.f, 0xffffff); }
}

/* a chrome footswitch on its nut */
static void footswitch(int cx, int cy, int pressed)
{
    float hx[12];
    for (int i = 0; i < 6; i++) hx[2 * i] = cx + 26 * cosf(i * TFX_PI / 3), hx[2 * i + 1] = cy + 26 * sinf(i * TFX_PI / 3);
    gfx_poly(hx, 6, 0xb9bbbe);
    gfx_disc(cx, cy, 21.f, 0x8e9094);
    gfx_disc(cx, cy, 17.f, pressed ? 0xc7c9cc : 0xe2e4e6);
    gfx_disc(cx - 5, cy - 6, 7.f, 0xf8f9fa);
    gfx_ring(cx, cy, 17.f, 1.5f, 0x6f7175);
}

/* a strip of Dymo tape: raised white capitals on black */
static void dymo(int cx, int y, const char *s, uint32_t tape)
{
    char b[48];
    snprintf(b, sizeof b, "%s", s);
    fit(b, PED_W - 30);
    int w = gfx_text_w(b, FONT_SMALL) + 20;
    gfx_fill(cx - w / 2 + 2, y + 2, w, 26, 0x55575b);
    gfx_fill(cx - w / 2, y, w, 26, tape);
    gfx_fill(cx - w / 2, y + 3, w, 1, gfx_mix(tape, 0xffffff, 0.12f));
    text_c(cx, y + 2, b, 0xf2f2f2, FONT_SMALL);
}

/* a Janus-style joystick: the gimbal plate, the boot, the stick leaning where it is pushed */
static void joystick(int cx, int cy, int sz, float jx, float jy, uint32_t c)
{
    int h = sz / 2;
    gfx_fill(cx - h + 3, cy - h + 3, sz, sz, 0x6f7174);
    gfx_fill(cx - h, cy - h, sz, sz, 0x141518);
    for (int i = 0; i < 4; i++) /* its screws */
        gfx_disc(cx + (i & 1 ? h - 9 : -h + 9), cy + (i & 2 ? h - 9 : -h + 9), 3.f, 0x3a3c40);
    gfx_disc(cx, cy, h - 8, 0x0a0a0c);
    gfx_ring(cx, cy, h - 8, 3.f, 0x2b2d31);
    gfx_disc(cx, cy, h - 18, 0x1d1e22); /* the rubber boot */
    gfx_ring(cx, cy, h * 0.45f, 1.5f, 0x26272b);
    float tx = cx + jx * (h - 22), ty = cy - jy * (h - 22);
    gfx_line(cx, cy, tx, ty, 9.f, 0x2e3035);
    gfx_disc(tx + 2, ty + 3, 12.f, 0x050506);
    gfx_disc(tx, ty, 12.f, 0x2a2b30);
    gfx_disc(tx - 3, ty - 4, 5.f, 0x4a4c52);
    gfx_ring(tx, ty, 12.f, 2.f, c);
}

static void hslider(int x, int y, int w, float stored, float lv, uint32_t col_, uint32_t axc, int thick)
{
    gfx_fill(x, y, w, thick, 0x0e1119);
    gfx_fill(x, y, (int)(w * stored), thick, gfx_mix(C_LINE, col_, 0.7f));
    gfx_fill(x + (int)(w * stored) - 2, y - 3, 4, thick + 6, col_);
    if (fabsf(lv - stored) > 0.004f) gfx_fill(x + (int)(w * lv) - 2, y - 5, 4, thick + 10, axc);
}

/* which stick axis moves parameter k of module m now (NULL: none; k -1: MIX) */
static const char *axis_of(int m, int k, uint32_t *c)
{
    if (!stick_matrix) {
        if (m == focus_mod && k >= 0 && k < 4) { *c = AXCOL[k]; return AXN[k]; }
        return NULL;
    }
    for (int ax = 0; ax < NAX; ax++)
        for (int s = 0; s < 2; s++)
            if (matrix[ax][s].mod == m && matrix[ax][s].param == k) { *c = AXCOL[ax]; return AXN[ax]; }
    return NULL;
}

/* a short stamped label: up to n letters */
static void short_name(const char *s, char *o, int n)
{
    int i = 0;
    for (; s[i] && i < n; i++) o[i] = s[i];
    o[i] = 0;
}

static void pedal(int x, int m)
{
    static const char *const CODE[NMOD] = {"TF0101", "TF0401", "TF0501", "TF0901"};
    const ModInfo *mi = eng_info(m);
    const ModState *ms = &base.m[m];
    int a = ms->algo, on = !ms->bypass, focused = col == m && !overlay, s = sel[m];
    int y = PED_Y;
    if (focused) gfx_rect(x - 4, y - 4, PED_W + 8, PED_H + 8, 3, 0xffffff);
    char b[64], v[48];

    /* the maker and the box's name, stamped across the top */
    stamp(x + 14, y + 10, "TRIMFX", 0, 2);
    snprintf(b, sizeof b, "%s", mi->name);
    stamp(x + PED_W - 14 - stamp_w(b, 0, 2), y + 10, b, 0, 2);

    /* the four big knobs (the sticks' four) */
    for (int k = 0; k < 4; k++) {
        int cx = x + PED_W / 4 + (k & 1) * PED_W / 2 + 1, cy = y + 92 + (k >> 1) * 128;
        int used = param_used(m, k);
        uint32_t axc = ACCENT;
        const char *ax = axis_of(m, k, &axc);
        knob(cx, cy, 27, ms->p[a][k], live.m[m].p[a][k], axc, used, focused && s == R_K1 + k);
        snprintf(b, sizeof b, "%s", used ? mi->pname[a][k] : "");
        fit(b, PED_W / 2 - 4);
        stamp_c(cx, cy + 40, b, 0, 1);
        if (ax) { /* which stick: a small painted dot and its letters */
            gfx_disc(cx - 36, cy - 30, 5.f, axc);
            stamp(cx - 30, cy - 41, ax, 0, 0);
        }
    }

    /* the four small knobs */
    for (int k = 4; k < 8; k++) {
        int cx = x + 28 + (k - 4) * 48, cy = y + 334, used = param_used(m, k);
        uint32_t axc = ACCENT;
        axis_of(m, k, &axc);
        knob(cx, cy, 15, ms->p[a][k], live.m[m].p[a][k], axc, used, focused && s == R_K1 + k);
        short_name(used ? mi->pname[a][k] : "", b, 6);
        stamp_c(cx, cy + ((k & 1) ? 44 : 23), b, 0, 0); /* staggered, so neighbours never touch */
    }

    /* the algorithm: a big selector knob, its name stamped under it */
    int acx = x + 58, acy = y + 462;
    float apos = mi->nalgo > 1 ? (float)a / (mi->nalgo - 1) : 0.f;
    for (int i = 0; i < mi->nalgo; i++) { /* a punched dot for each position */
        float px, py;
        knob_pt(acx, acy, 46, mi->nalgo > 1 ? (float)i / (mi->nalgo - 1) : 0.f, &px, &py);
        gfx_disc(px + 1, py + 1, 2.f, INK_LIGHT);
        gfx_disc(px, py, 2.f, i == a ? ACCENT : INK);
    }
    knob(acx, acy, 34, apos, apos, ACCENT, 1, focused && s == R_ALGO);
    snprintf(v, sizeof v, "%d", a + 1);
    stamp_c(acx, acy + 50, v, 0, 0);

    /* MIX */
    uint32_t axc = ACCENT;
    axis_of(m, -1, &axc);
    knob(x + 150, acy - 8, 24, ms->mix, live.m[m].mix, axc, 1, focused && s == R_MIX);
    stamp_c(x + 150, acy + 26, "MIX", 0, 2);

    /* the name of the algorithm, then the Dymo readout */
    snprintf(b, sizeof b, "%s", mi->algo[a]);
    stamp_c(x + PED_W / 2, y + 538, b, 0, 2);
    if (focused && s >= R_K1 && s <= R_D4 && param_used(m, s - R_K1)) {
        int k = s - R_K1;
        mi->fmt(a, k, live.m[m].p[a][k], &live, v, sizeof v);
        snprintf(b, sizeof b, "%s %s", mi->pname[a][k], v);
    } else if (focused && s == R_MIX)
        snprintf(b, sizeof b, ms->mix >= 0.999f ? "MIX ALL WET" : "MIX %.0f%%", live.m[m].mix * 100.f);
    else if (focused && s == R_ALGO)
        snprintf(b, sizeof b, "%d/%d %s", a + 1, mi->nalgo, mi->algo[a]);
    else
        snprintf(b, sizeof b, "%s", mi->blurb[a]);
    dymo(x + PED_W / 2, y + 566, b, focused ? 0x14161a : 0x2a2c31);

    /* the LED, the footswitch, the serial */
    int frozen = (m == M_DELAY && base.freeze_dly) || (m == M_VERB && base.freeze_verb);
    led(x + 146, y + 622, frozen ? 0x4fb8ff : 0xff2b1a, on || frozen);
    footswitch(x + 146, y + 676, !on);
    stamp(x + 18, y + 668, CODE[m], 0, 2);
    if (frozen) stamp(x + 18, y + 620, "FROZEN", 0, 1);
    else if (!on) stamp(x + 18, y + 620, "BYPASS", 0, 1);
    /* the level, a thin bar under the readout */
    float t = clampf((lin2db(vu_mod[m] + 1e-9f) + 48.f) / 48.f, 0.f, 1.f);
    gfx_fill(x + 30, y + 598, PED_W - 60, 3, 0x7a7c80);
    gfx_fill(x + 30, y + 598, (int)((PED_W - 60) * t), 3, on ? 0x2f3135 : 0x8a8c90);
}

static void master_strip(int x, int w)
{
    int focused = col == COL_MASTER && !overlay, y = PED_Y;
    if (focused) gfx_rect(x - 4, y - 4, w + 8, PED_H + 8, 3, 0xffffff);
    stamp_c(x + w / 2, y + 10, "TRIMFX", 1, 2);
    stamp_c(x + w / 2, y + 52, "IN  FX  OUT", 0, 0);
    int ry0 = y + 86;
    for (int r = 0; r < NMROW; r++) {
        int ry = ry0 + r * 29, here = focused && sel[COL_MASTER] == r;
        char lab[24], v[48];
        switch (r) {
        case MR_IN: snprintf(lab, sizeof lab, "IN"); snprintf(v, sizeof v, "%+.1f", base.in_db); break;
        case MR_OUT: snprintf(lab, sizeof lab, "OUT"); snprintf(v, sizeof v, "%+.1f", base.out_db); break;
        case MR_DRY: snprintf(lab, sizeof lab, "DRY"); snprintf(v, sizeof v, base.dry < 0.005f ? "OFF" : "%.0f%%", base.dry * 100.f); break;
        case MR_TEMPO: snprintf(lab, sizeof lab, "BPM"); snprintf(v, sizeof v, "%.0f", base.bpm); break;
        case MR_ORDER: snprintf(lab, sizeof lab, "ORDER"); snprintf(v, sizeof v, "%d", base.order + 1); break;
        case MR_SLOT: snprintf(lab, sizeof lab, "PRESET"); snprintf(v, sizeof v, "%d", preset_slot + 1); break;
        case MR_SETUP: snprintf(lab, sizeof lab, "SETUP"); snprintf(v, sizeof v, ">"); break;
        default: snprintf(lab, sizeof lab, "STICKS"); snprintf(v, sizeof v, ">"); break;
        }
        if (here) gfx_rect(x + 6, ry - 3, w - 12, 27, 2, ACCENT);
        stamp(x + 12, ry, lab, 0, 1);
        stamp(x + w - 12 - stamp_w(v, 0, 1), ry, v, 0, 1);
    }
    /* the preset's name on tape, and the chain's order */
    int ny = ry0 + NMROW * 29 + 6;
    char nm[48];
    snprintf(nm, sizeof nm, "%s", eng_order_name(base.order));
    stamp_c(x + w / 2, ny, nm, 0, 0);
    snprintf(nm, sizeof nm, "%s", patch_name);
    fit(nm, w - 30);
    int tw = gfx_text_w(nm, FONT_SMALL) + 16;
    gfx_fill(x + w / 2 - tw / 2, ny + 26, tw, 24, 0x14161a);
    text_c(x + w / 2, ny + 27, nm, 0xf2f2f2, FONT_SMALL);
    /* the two joysticks: where the sticks are, and what they move */
    float av[NAX];
    axes(av);
    int m = focus_mod, a = base.m[m].algo, sz = w - 62;
    for (int s = 0; s < 2; s++) {
        int cy = ny + 66 + sz / 2 + s * (sz + 42);
        joystick(x + w / 2, cy, sz, av[2 * s], av[2 * s + 1], stick_matrix ? 0x8a8c90 : AXCOL[2 * s]);
        char lab[40];
        if (stick_matrix) snprintf(lab, sizeof lab, "%s MATRIX", s ? "R" : "L");
        else snprintf(lab, sizeof lab, "%.5s/%.5s", eng_info(m)->pname[a][2 * s], eng_info(m)->pname[a][2 * s + 1]);
        stamp_c(x + w / 2, cy + sz / 2 + 8, lab, 0, 0);
    }
    /* the levels: a light each side of the maker's name, in and out */
    float li = fmaxf(vu_in[0], vu_in[1]), lo = fmaxf(vu_out[0], vu_out[1]);
    led(x + 18, y + 62, li > 0.9f ? 0xff2b1a : 0x43e05a, li > 0.01f);
    led(x + w - 18, y + 62, lo > 0.9f ? 0xff2b1a : 0x43e05a, lo > 0.01f);
}

static void draw_top(void)
{
    char b[96];
    snprintf(b, sizeof b, "%d  %s   %.0f BPM%s", preset_slot + 1, patch_name, base.bpm, in_dev ? "" : "   NO INPUT");
    int w = gfx_text_w(b, FONT_SMALL) + 20;
    gfx_fill(10, 5, w, 26, 0x14161a);
    gfx_text(20, 7, b, in_dev ? 0xf2f2f2 : 0xff8080, FONT_SMALL);
    snprintf(b, sizeof b, "STICKS %s %s%s", stick_matrix ? "MATRIX" : "FOCUS", stick_latch ? "LATCH" : "SPRING", hold_on ? " HELD" : "");
    w = gfx_text_w(b, FONT_SMALL) + 20;
    gfx_fill(W - 10 - w, 5, w, 26, 0x14161a);
    text_r(W - 20, 7, b, hold_on ? ACCENT : 0xf2f2f2, FONT_SMALL);
}

static void row_box(int x, int y, int w, int here, const char *label, const char *val, uint32_t c, const char *axn, uint32_t axc, float bar)
{
    gfx_fill(x, y, w, 32, here ? C_PANEL2 : C_PANEL);
    if (here) gfx_fill(x, y, 4, 32, c);
    gfx_text(x + 14, y + 5, label, here ? C_TEXT : C_DIM, FONT_SMALL);
    if (axn) { gfx_fill(x + 150, y + 6, 30, 20, gfx_mix(C_PANEL, axc, 0.35f)); text_c(x + 165, y + 5, axn, axc, FONT_SMALL); }
    if (bar >= 0.f) hslider(x + 196, y + 14, 150, bar, bar, c, c, 5);
    char v[80];
    snprintf(v, sizeof v, "%s", val);
    fit(v, w - (bar >= 0.f ? 370 : 200));
    text_r(x + w - 10, y + 5, v, here ? C_TEXT : C_DIM, FONT_SMALL);
}

static void draw_overlay(void)
{
    int x = 92, y = 70, w = 840, h = 620;
    gfx_fill_a(0, 0, W, H, 0x000000, 150);
    gfx_fill(x, y, w, h, C_BG);
    gfx_rect(x, y, w, h, 2, C_ACC);
    text_c(x + w / 4, y + 10, "SETUP", overlay == OV_SETUP ? C_TEXT : C_DIM, FONT_LARGE);
    text_c(x + 3 * w / 4, y + 10, "STICKS", overlay == OV_STICKS ? C_TEXT : C_DIM, FONT_LARGE);
    gfx_fill(overlay == OV_SETUP ? x + 40 : x + w / 2 + 40, y + 50, w / 2 - 80, 3, C_ACC);
    gfx_text(x + 16, y + h - 30, "L1/R1 tab   up/down choose   left/right change   A do   B close", C_DIM, FONT_SMALL);
    int ry = y + 72;
    char lab[48], v[96];
    if (overlay == OV_SETUP) {
        for (int r = 0; r < NSROW; r++) {
            switch (r) {
            case SR_INPUT: snprintf(lab, sizeof lab, "INPUT"); snprintf(v, sizeof v, "%s", in_dev ? (*in_name ? in_name : "DEFAULT") : "NONE"); break;
            case SR_OUTPUT: snprintf(lab, sizeof lab, "OUTPUT"); snprintf(v, sizeof v, "%s", *out_name ? out_name : "DEFAULT"); break;
            case SR_SAVE: snprintf(lab, sizeof lab, "SAVE TO SLOT"); snprintf(v, sizeof v, "%d  (A)", preset_slot + 1); break;
            case SR_LOAD: snprintf(lab, sizeof lab, "LOAD SLOT"); snprintf(v, sizeof v, "%d  (A)", preset_slot + 1); break;
            case SR_FACTORY: { Patch tmp; snprintf(lab, sizeof lab, "FACTORY"); snprintf(v, sizeof v, "%d %s  (A)", factory_pick + 1, patch_factory(&tmp, factory_pick)); } break;
            default: snprintf(lab, sizeof lab, "INIT ALL"); snprintf(v, sizeof v, "A, twice"); break;
            }
            row_box(x + 16, ry + r * 38, w - 32, ov_sel[OV_SETUP] == r, lab, v, C_ACC, NULL, 0, -1.f);
        }
        const char *lines[] = {
            "FX SEND: plug a USB interface into the top port. Its input is",
            "the send, its output the return. MASTER > DRY off = all wet;",
            "each pedal's MIX goes to 100 % wet. DELAY and REVERB bypass",
            "with trails. INPUT / OUTPUT: AUTO picks the first USB device.",
        };
        for (int i = 0; i < 4; i++) gfx_text(x + 24, ry + NSROW * 38 + 20 + i * 24, lines[i], C_DIM, FONT_SMALL);
        snprintf(v, sizeof v, "TrimFX %s  %s  buffer %d  underruns %u  overruns %u", TFX_VERSION, is_pro ? "Brick Pro" : "Brick", buffer,
                 atomic_load(&underruns), atomic_load(&overruns));
        gfx_text(x + 24, ry + NSROW * 38 + 130, v, C_DIM, FONT_SMALL);
        return;
    }
    /* STICKS: the matrix on the left, the sticks on the right */
    int vis = 13, r = ov_sel[OV_STICKS];
    if (r < ov_scroll[OV_STICKS]) ov_scroll[OV_STICKS] = r;
    if (r >= ov_scroll[OV_STICKS] + vis) ov_scroll[OV_STICKS] = r - vis + 1;
    for (int i = ov_scroll[OV_STICKS]; i < NXROW && i < ov_scroll[OV_STICKS] + vis; i++) {
        uint32_t ac = C_ACC;
        const char *axn = NULL;
        float bar = -1.f;
        if (i == 0) snprintf(lab, sizeof lab, "MODE"), snprintf(v, sizeof v, stick_matrix ? "MATRIX" : "FOCUS");
        else if (i == 1) snprintf(lab, sizeof lab, "BEHAVIOUR"), snprintf(v, sizeof v, stick_latch ? "LATCH" : "SPRING");
        else if (i == 2) snprintf(lab, sizeof lab, "RESET MATRIX"), snprintf(v, sizeof v, "< >");
        else {
            int q = i - 3, ax = q / 4, s = (q / 2) % 2;
            Target *t = &matrix[ax][s];
            ac = AXCOL[ax];
            axn = AXN[ax];
            if (q & 1) { snprintf(lab, sizeof lab, " DEPTH %d", s + 1); snprintf(v, sizeof v, "%+.0f%%", t->depth * 100.f); bar = 0.5f + 0.5f * t->depth; }
            else { snprintf(lab, sizeof lab, "TARGET %d", s + 1); target_name(*t, v, sizeof v); }
        }
        row_box(x + 16, ry + (i - ov_scroll[OV_STICKS]) * 38, 540, i == r, lab, v, ac, axn, ac, bar);
    }
    float av[NAX];
    axes(av);
    for (int s = 0; s < 2; s++) {
        int px = x + 600, py = ry + 24 + s * 250;
        gfx_text(px, py - 22, s ? "RIGHT STICK" : "LEFT STICK", C_TEXT, FONT_SMALL);
        xy_pad(px, py, 200, 0.5f, 0.5f, 0.5f + 0.5f * av[2 * s], 0.5f + 0.5f * av[2 * s + 1], AXCOL[2 * s], AXN[2 * s], AXN[2 * s + 1], 1);
    }
}

static void draw(void)
{
    if (!bg) make_bg();
    if (bg) memcpy(surf->pixels, bg, sizeof(uint32_t) * W * H); /* the board and the bare boxes */
    else gfx_clear(BOARD);
    draw_top();
    int ord[4] = {M_TONE, 0, 0, 0};
    eng_order(base.order, ord + 1);
    /* the boxes in signal order, then the master box */
    for (int k = 0; k < 4; k++) pedal(PED_X(k), ord[k]);
    master_strip(MST_X, MST_W);
    if (overlay) draw_overlay();
    if (SDL_GetTicks() < toast_until) {
        int w = gfx_text_w(toast_msg, FONT_LARGE) + 40;
        gfx_fill_a(W / 2 - w / 2, H / 2 - 30, w, 54, 0x000000, 225);
        gfx_rect(W / 2 - w / 2, H / 2 - 30, w, 54, 2, ACCENT);
        text_c(W / 2, H / 2 - 21, toast_msg, C_TEXT, FONT_LARGE);
    }
}

/* ================================================================ the screen */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
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
    vu_in[0] = vu_in[1] = 0.3f; vu_out[0] = 0.5f; vu_out[1] = 0.45f;
    for (int m = 0; m < NMOD; m++) vu_mod[m] = 0.2f + 0.1f * m;
    static const struct { int col, sel, ov; const char *tag; } S[] = {
        {COL_MOD, R_K1, OV_NONE, "board_mod"}, {COL_VERB, R_D2, OV_NONE, "board_verb"}, {COL_MASTER, MR_TEMPO, OV_NONE, "board_master"},
        {COL_MASTER, MR_SETUP, OV_SETUP, "setup"}, {COL_MASTER, MR_STICKS, OV_STICKS, "sticks"},
    };
    for (unsigned i = 0; i < sizeof S / sizeof *S; i++) {
        set_col(S[i].col);
        sel[col] = S[i].sel;
        overlay = S[i].ov;
        ov_sel[OV_STICKS] = 3;
        build_live(0.f);
        draw();
        shot(path, S[i].tag);
    }
    overlay = OV_NONE;
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
    if (!is_pro) toast("NO STICKS HERE: KNOBS ONLY (OR A GAMEPAD)");

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
