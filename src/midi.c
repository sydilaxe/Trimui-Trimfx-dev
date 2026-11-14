/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "midi.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 4
typedef struct {
    int fd;
    char path[256], name[48];
    uint8_t status, d[2];
    int nd, sysex;
} Dev;
static Dev dev[MAXDEV];
static int ndev;
static double next_scan;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void card_name(const char *path, char *out, int n)
{
    int card = -1, d = 0;
    snprintf(out, n, "MIDI");
    if (sscanf(path, "/dev/snd/midiC%dD%d", &card, &d) < 1) return;
    char p[64];
    snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, n, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

static void scan(void)
{
    glob_t g;
    const char *pat = getenv("TFX_MIDI"); /* another device pattern (tests: a FIFO) */
    if (glob(pat && *pat ? pat : "/dev/snd/midiC*D*", 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc && ndev < MAXDEV; i++) {
        int have = 0;
        for (int k = 0; k < ndev; k++)
            if (!strcmp(dev[k].path, g.gl_pathv[i])) have = 1;
        if (have) continue;
        int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        Dev *d = &dev[ndev++];
        memset(d, 0, sizeof *d);
        d->fd = fd;
        snprintf(d->path, sizeof d->path, "%s", g.gl_pathv[i]);
        card_name(d->path, d->name, sizeof d->name);
        fprintf(stderr, "midi: opened %s (%s)\n", d->path, d->name);
    }
    globfree(&g);
}

static void drop(int k)
{
    fprintf(stderr, "midi: %s gone\n", dev[k].path);
    close(dev[k].fd);
    dev[k] = dev[--ndev];
}

/* one byte through the parser; 1 when an event is complete */
static int parse(Dev *d, uint8_t b, MidiEv *e)
{
    if (b >= 0xf8) return 0;                 /* real-time: ignore, keep running status */
    if (b == 0xf0) { d->sysex = 1; return 0; }
    if (b >= 0xf0) { d->sysex = 0; d->status = 0; return 0; }
    if (b & 0x80) { d->sysex = 0; d->status = b; d->nd = 0; return 0; }
    if (d->sysex || !d->status) return 0;
    int hi = d->status & 0xf0, need = (hi == 0xc0 || hi == 0xd0) ? 1 : 2;
    d->d[d->nd++] = b;
    if (d->nd < need) return 0;
    d->nd = 0;
    e->ch = d->status & 15;
    if (hi == 0xb0) { e->note = 0; e->num = d->d[0]; e->val = d->d[1]; return 1; }
    if (hi == 0x90 || hi == 0x80) { e->note = 1; e->num = d->d[0]; e->val = hi == 0x80 ? 0 : d->d[1]; return 1; }
    return 0;
}

int midi_poll(MidiEv *out, int max)
{
    double t = now_s();
    if (t >= next_scan) { scan(); next_scan = t + 2.0; }
    int n = 0;
    for (int k = 0; k < ndev && n < max; k++) {
        uint8_t buf[256];
        ssize_t got = read(dev[k].fd, buf, sizeof buf);
        if (got < 0) {
            if (errno != EAGAIN && errno != EINTR) { drop(k); k--; }
            continue;
        }
        if (got == 0) continue;
        for (ssize_t i = 0; i < got; i++) {
            MidiEv e;
            if (parse(&dev[k], buf[i], &e) && n < max) out[n++] = e;
        }
    }
    return n;
}

int midi_count(void) { return ndev; }
const char *midi_name(void) { return ndev ? dev[0].name : ""; }

void midi_close(void)
{
    while (ndev) { close(dev[ndev - 1].fd); ndev--; }
}
