/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Full duplex on one USB interface: see duplex.h. */
#define _GNU_SOURCE
#include "duplex.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- libasound, loaded at run time */
typedef struct snd_pcm snd_pcm_t;
typedef struct snd_hw snd_pcm_hw_params_t;
typedef struct snd_sw snd_pcm_sw_params_t;
typedef unsigned long uframes;
typedef long sframes;
enum { ST_PLAYBACK = 0, ST_CAPTURE = 1, ACCESS_RW_INTERLEAVED = 3, FORMAT_FLOAT_LE = 14 };

static struct {
    void *lib;
    int (*open)(snd_pcm_t **, const char *, int, int);
    int (*close)(snd_pcm_t *);
    int (*hw_malloc)(snd_pcm_hw_params_t **);
    void (*hw_free)(snd_pcm_hw_params_t *);
    int (*hw_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
    int (*hw_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*hw_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*hw_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
    int (*hw_resample)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
    int (*hw_rate)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *, int *);
    int (*hw_period)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *, int *);
    int (*hw_buffer)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *);
    int (*hw_apply)(snd_pcm_t *, snd_pcm_hw_params_t *);
    int (*hw_get_period)(const snd_pcm_hw_params_t *, uframes *, int *);
    int (*hw_get_buffer)(const snd_pcm_hw_params_t *, uframes *);
    int (*sw_malloc)(snd_pcm_sw_params_t **);
    void (*sw_free)(snd_pcm_sw_params_t *);
    int (*sw_current)(snd_pcm_t *, snd_pcm_sw_params_t *);
    int (*sw_start)(snd_pcm_t *, snd_pcm_sw_params_t *, uframes);
    int (*sw_avail)(snd_pcm_t *, snd_pcm_sw_params_t *, uframes);
    int (*sw_apply)(snd_pcm_t *, snd_pcm_sw_params_t *);
    int (*prepare)(snd_pcm_t *);
    int (*start)(snd_pcm_t *);
    int (*drop)(snd_pcm_t *);
    int (*link)(snd_pcm_t *, snd_pcm_t *);
    int (*unlink)(snd_pcm_t *);
    sframes (*readi)(snd_pcm_t *, void *, uframes);
    sframes (*writei)(snd_pcm_t *, const void *, uframes);
    const char *(*strerror)(int);
} A;

static int load(char *err, int n)
{
    if (A.lib) return 0;
    void *l = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!l) { snprintf(err, (size_t)n, "no libasound (%s)", dlerror()); return -1; }
#define SYM(f, name) if (!(*(void **)&A.f = dlsym(l, name))) { snprintf(err, (size_t)n, "libasound lacks %s", name); dlclose(l); return -1; }
    SYM(open, "snd_pcm_open") SYM(close, "snd_pcm_close")
    SYM(hw_malloc, "snd_pcm_hw_params_malloc") SYM(hw_free, "snd_pcm_hw_params_free") SYM(hw_any, "snd_pcm_hw_params_any")
    SYM(hw_access, "snd_pcm_hw_params_set_access") SYM(hw_format, "snd_pcm_hw_params_set_format")
    SYM(hw_channels, "snd_pcm_hw_params_set_channels") SYM(hw_resample, "snd_pcm_hw_params_set_rate_resample")
    SYM(hw_rate, "snd_pcm_hw_params_set_rate_near") SYM(hw_period, "snd_pcm_hw_params_set_period_size_near")
    SYM(hw_buffer, "snd_pcm_hw_params_set_buffer_size_near") SYM(hw_apply, "snd_pcm_hw_params")
    SYM(hw_get_period, "snd_pcm_hw_params_get_period_size") SYM(hw_get_buffer, "snd_pcm_hw_params_get_buffer_size")
    SYM(sw_malloc, "snd_pcm_sw_params_malloc") SYM(sw_free, "snd_pcm_sw_params_free") SYM(sw_current, "snd_pcm_sw_params_current")
    SYM(sw_start, "snd_pcm_sw_params_set_start_threshold") SYM(sw_avail, "snd_pcm_sw_params_set_avail_min")
    SYM(sw_apply, "snd_pcm_sw_params")
    SYM(prepare, "snd_pcm_prepare") SYM(start, "snd_pcm_start") SYM(drop, "snd_pcm_drop")
    SYM(link, "snd_pcm_link") SYM(unlink, "snd_pcm_unlink")
    SYM(readi, "snd_pcm_readi") SYM(writei, "snd_pcm_writei") SYM(strerror, "snd_strerror")
#undef SYM
    A.lib = l;
    return 0;
}

/* ---------------------------------------------------------------- the streams */
static DxConfig cfg;
static snd_pcm_t *cap, *play;
static int frames, periods, linked;
static float *cbuf, *pbuf, *silence;
static pthread_t thr;
static int thr_on;
static atomic_int run, alive;
static atomic_uint xruns, done;

/* one direction: float, the interface's channels, 48 kHz exactly (no resampling here: an
 * interface that can't do 48 kHz stays on the SDL path, whose conversion is better), period;
 * the buffer: the output's kept to what it queues, the input's roomier (it costs no latency:
 * each period is read the moment it is in) */
static int setup(snd_pcm_t *p, int ch, unsigned want_rate, uframes *period, uframes buffer, char *err, int n, const char *what)
{
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    unsigned rate = want_rate;
    int dir = 0, e;
    uframes buf = buffer;
    if ((e = A.hw_malloc(&hw)) < 0) goto fail;
    if ((e = A.hw_any(p, hw)) < 0 || (e = A.hw_access(p, hw, ACCESS_RW_INTERLEAVED)) < 0 || (e = A.hw_format(p, hw, FORMAT_FLOAT_LE)) < 0 ||
        (e = A.hw_channels(p, hw, (unsigned)ch)) < 0 || (e = A.hw_resample(p, hw, 0)) < 0 || (e = A.hw_rate(p, hw, &rate, &dir)) < 0)
        goto fail;
    if (rate != want_rate) { snprintf(err, (size_t)n, "%s: the interface runs %u Hz, not %u", what, rate, want_rate); A.hw_free(hw); return -1; }
    if ((e = A.hw_period(p, hw, period, &dir)) < 0 || (e = A.hw_buffer(p, hw, &buf)) < 0 || (e = A.hw_apply(p, hw)) < 0) goto fail;
    A.hw_get_period(hw, period, &dir);
    A.hw_free(hw), hw = NULL;
    if ((e = A.sw_malloc(&sw)) < 0 || (e = A.sw_current(p, sw)) < 0) goto fail;
    A.sw_start(p, sw, (uframes)1 << 30); /* never by itself: dx starts both together */
    A.sw_avail(p, sw, *period);
    if ((e = A.sw_apply(p, sw)) < 0) goto fail;
    A.sw_free(sw);
    return 0;
fail:
    snprintf(err, (size_t)n, "%s: %s", what, A.strerror(e));
    if (hw) A.hw_free(hw);
    if (sw) A.sw_free(sw);
    return -1;
}

/* both from the top: silence queued on the output, then both started together */
static int restart(void)
{
    if (cap && linked) A.drop(cap);
    else {
        if (cap) A.drop(cap);
        A.drop(play);
    }
    if (A.prepare(play) < 0 || (cap && A.prepare(cap) < 0)) return -1;
    for (int k = 0; k < periods; k++)
        if (A.writei(play, silence, (uframes)frames) < 0) return -1;
    if (cap && linked) return A.start(cap);
    int e = A.start(play);
    return cap ? A.start(cap) : e;
}

static int gone(int e) { return e == -ENODEV || e == -EBADFD || e == -EIO || e == -ENOENT; }

static void *loop(void *arg)
{
    (void)arg;
    if (cfg.on_thread) cfg.on_thread();
    while (atomic_load_explicit(&run, memory_order_relaxed)) {
        /* a whole period in (playback only: none; the output's own pace times the loop) */
        int got = 0, e = 0;
        while (cap && got < frames) {
            sframes r = A.readi(cap, cbuf + (size_t)got * (size_t)cfg.ch_in, (uframes)(frames - got));
            if (r < 0) { e = (int)r; break; }
            got += (int)r;
        }
        if (!e) {
            cfg.period(cbuf, pbuf, frames, cfg.ud);
            int put = 0;
            while (put < frames) {
                sframes w = A.writei(play, pbuf + (size_t)put * (size_t)cfg.ch_out, (uframes)(frames - put));
                if (w < 0) { e = (int)w; break; }
                put += (int)w;
            }
        }
        if (e == -EINTR || e == -EAGAIN) continue;
        if (e) {
            if (gone(e) || !atomic_load(&run)) break;
            atomic_fetch_add(&xruns, 1); /* -EPIPE / -ESTRPIPE: a period missed */
            if (restart() < 0) break;
            continue;
        }
        atomic_fetch_add_explicit(&done, 1, memory_order_relaxed);
    }
    atomic_store(&alive, 0);
    return NULL;
}

static void close_all(void)
{
    if (cap && linked) A.unlink(cap);
    if (cap) A.close(cap), cap = NULL;
    if (play) A.close(play), play = NULL;
    free(cbuf), free(pbuf), free(silence);
    cbuf = pbuf = silence = NULL;
    linked = 0;
}

int dx_start(const DxConfig *c, char *err, int n)
{
    dx_stop();
    if (load(err, n) < 0) return -1;
    cfg = *c;
    periods = c->periods >= 3 ? 3 : 2;
    char dev[96];
    if (!strncmp(c->card, "pcm:", 4)) snprintf(dev, sizeof dev, "%s", c->card + 4); /* (tests: any PCM, e.g. "pcm:null") */
    else snprintf(dev, sizeof dev, "plughw:CARD=%s,DEV=0", c->card);
    int e;
    if ((e = A.open(&play, dev, ST_PLAYBACK, 0)) < 0) { snprintf(err, (size_t)n, "output %s: %s", dev, A.strerror(e)); play = NULL; close_all(); return -1; }
    if (c->ch_in > 0 && (e = A.open(&cap, dev, ST_CAPTURE, 0)) < 0) { snprintf(err, (size_t)n, "input %s: %s", dev, A.strerror(e)); cap = NULL; close_all(); return -1; }
    unsigned rate = c->rate > 0 ? (unsigned)c->rate : 48000u;
    uframes pp = (uframes)c->frames, cp = (uframes)c->frames;
    if (setup(play, c->ch_out, rate, &pp, pp * (uframes)periods, err, n, "output") < 0 ||
        (cap && setup(cap, c->ch_in, rate, &cp, cp * 8, err, n, "input") < 0)) {
        close_all();
        return -1;
    }
    if (!cap) cp = pp;
    if (pp != cp) { snprintf(err, (size_t)n, "the periods differ (output %lu, input %lu)", pp, cp); close_all(); return -1; }
    frames = (int)pp;
    cbuf = calloc((size_t)frames * (size_t)(c->ch_in > 0 ? c->ch_in : 1), sizeof(float));
    pbuf = calloc((size_t)frames * (size_t)c->ch_out, sizeof(float));
    silence = calloc((size_t)frames * (size_t)c->ch_out, sizeof(float));
    if (!cbuf || !pbuf || !silence) { snprintf(err, (size_t)n, "out of memory"); close_all(); return -1; }
    linked = cap && A.link(cap, play) == 0;
    atomic_store(&xruns, 0), atomic_store(&done, 0);
    if (restart() < 0) { snprintf(err, (size_t)n, "could not start the streams"); close_all(); return -1; }
    atomic_store(&run, 1), atomic_store(&alive, 1);
    if (pthread_create(&thr, NULL, loop, NULL) == 0) thr_on = 1;
    else {
        atomic_store(&run, 0), atomic_store(&alive, 0);
        snprintf(err, (size_t)n, "no thread");
        close_all();
        return -1;
    }
    if (cfg.log)
    {
        if (cap)
            cfg.log("AUDIO", "DUPLEX on %s: %u Hz, %d frames a period, %d queued out, %s; round trip ~%.1f ms + the interface", dev, rate, frames,
                    periods, linked ? "linked (one start)" : "not linked", (double)frames * (1 + periods) * 1000.0 / rate);
        else
            cfg.log("AUDIO", "PLAYBACK on %s: %u Hz, %d frames a period, %d queued; out in ~%.1f ms + the interface", dev, rate, frames, periods,
                    (double)frames * periods * 1000.0 / rate);
    }
    return 0;
}

void dx_stop(void)
{
    if (!cap && !play) return;
    /* the thread wakes within a period (the streams keep running) and sees run 0 */
    atomic_store(&run, 0);
    if (thr_on) pthread_join(thr, NULL), thr_on = 0;
    atomic_store(&alive, 0);
    close_all();
}

int dx_alive(void) { return atomic_load(&alive); }
int dx_frames(void) { return frames; }
unsigned dx_xruns(void) { return atomic_load(&xruns); }
unsigned dx_periods_done(void) { return atomic_load_explicit(&done, memory_order_relaxed); }
