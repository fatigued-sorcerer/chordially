// SPDX-License-Identifier: GPL-3.0-only
// Chordially: an EXTERNAL part (the S-1, the T-8). Its USB audio comes in through the shell's audio-in (the device
// whose name contains DEVICE), is mixed with LEVEL / MUTE and sent to the delay and reverb. Its USB MIDI port gets
// the clock and start / stop (the clock list in the core; on by default), and optionally the notes played on THRU IN
// channel, re-sent on its own channel. Two instances: part 1 (S-1) and part 2 (T-8).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cm_int.h"

typedef struct {
    char dev[64];          /* the audio device (and MIDI port) name to look for: "S-1" */
    int level, mute, dly, rev, thru_in, thru_ch, present, slot;
    int audio, midi_in, start;   /* AUDIO: take its USB audio; MIDI IN: its keys play Chordially; SYNC ("start"): it gets the clock and START / STOP; off: nothing */
    char adev[128]; int ach;     /* the audio device chosen in the strip ("" = AUTO: the one named like dev, never an aggregate) and its first input */
    int held[128];
} ext_t;
static ext_t X[2];

static ext_t *E(part_t *p) { return (ext_t *)p->s; }
static void ex_init(part_t *p)
{
    int k = p->idx - 1; if (k < 0 || k > 1) k = 0;
    ext_t *e = &X[k]; memset(e, 0, sizeof *e);
    p->s = e; e->slot = k;
    snprintf(e->dev, sizeof e->dev, "%s", k == 0 ? "S-1" : "T-8");
    e->audio = 1; e->midi_in = 0; e->start = 0; e->adev[0] = 0; e->ach = 0;
    e->level = 100; e->dly = 0; e->rev = 0; e->thru_in = 0; e->thru_ch = k == 0 ? 1 : 10;
}
static void ex_render(part_t *p, float *L, float *R, float *sd, float *sr, int n)
{
    ext_t *e = E(p);
    float l[MAXF], r[MAXF];
    if (!cm_ain || !cm_ain(e->slot, n, l, r)) { p->peak *= 0.9f; return; }
    float g = e->mute ? 0.f : e->level / 100.f; g *= g;
    float dg = e->dly / 127.f, rg = e->rev / 127.f; dg *= dg; rg *= rg;
    float pk = 0;
    for (int i = 0; i < n; i++) {
        float a = l[i] * g, b = r[i] * g;
        L[i] += a; R[i] += b;
        float m = 0.5f * (a + b); sd[i] += m * dg; sr[i] += m * rg;
        float x = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b); if (x > pk) pk = x;
    }
    p->peak = pk > p->peak ? pk : p->peak * 0.92f;
}
static void ex_state(part_t *p, jb_t *b)
{
    ext_t *e = E(p);
    jb_key(b, "dev"); jb_str_utf8(b, e->dev);
#define F(k) jb_key(b, #k); jb_int(b, e->k);
    F(level) F(mute) F(dly) F(rev) F(thru_in) F(thru_ch) F(present) F(audio) F(midi_in) F(start) F(ach)
#undef F
    jb_key(b, "adev"); jb_str_utf8(b, e->adev);
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ex_set(part_t *p, const char *k, const js_t *m)
{
    ext_t *e = E(p);
    if (strcmp(k, "set")) return 0;
    const char *f = js_strv(m, "f"); int v = (int)js_num(m, "v", 0);
    if (!f) return 0;
    if (!strcmp(f, "level")) e->level = clampi(v, 0, 127);
    else if (!strcmp(f, "mute")) e->mute = !!v;
    else if (!strcmp(f, "dly")) e->dly = clampi(v, 0, 127);
    else if (!strcmp(f, "rev")) e->rev = clampi(v, 0, 127);
    else if (!strcmp(f, "thru_in")) e->thru_in = clampi(v, 0, 16);
    else if (!strcmp(f, "thru_ch")) e->thru_ch = clampi(v, 1, 16);
    else if (!strcmp(f, "audio")) e->audio = !!v;
    else if (!strcmp(f, "midi_in")) e->midi_in = !!v;
    else if (!strcmp(f, "start")) e->start = !!v;
    else if (!strcmp(f, "adev")) { const char *s = js_strv(m, "s"); snprintf(e->adev, sizeof e->adev, "%s", s ? s : ""); e->ach = clampi(v, 0, 62); }
    else if (!strcmp(f, "dev")) { const char *s = js_strv(m, "s"); if (s) snprintf(e->dev, sizeof e->dev, "%s", s); }
    else return 0;
    cm_dirty();
    return 1;
}
static void ex_save(part_t *p, jb_t *b) { ex_state(p, b); }
static void ex_load(part_t *p, const js_t *o)
{
    ext_t *e = E(p);
    if (!o) return;
    const char *d = js_strv(o, "dev"); if (d && *d) snprintf(e->dev, sizeof e->dev, "%s", d);
    e->level = clampi((int)js_num(o, "level", e->level), 0, 127); e->mute = (int)js_num(o, "mute", 0);
    e->dly = clampi((int)js_num(o, "dly", 0), 0, 127); e->rev = clampi((int)js_num(o, "rev", 0), 0, 127);
    e->audio = (int)js_num(o, "audio", 1) != 0; e->midi_in = (int)js_num(o, "midi_in", 0) != 0; e->start = (int)js_num(o, "start", 0) != 0;
    const char *ad = js_strv(o, "adev"); snprintf(e->adev, sizeof e->adev, "%s", ad ? ad : ""); e->ach = clampi((int)js_num(o, "ach", 0), 0, 62);
    e->thru_in = clampi((int)js_num(o, "thru_in", 0), 0, 16); e->thru_ch = clampi((int)js_num(o, "thru_ch", e->thru_ch), 1, 16);
}
static void ex_live(part_t *p, jb_t *b) { jb_key(b, "present"); jb_int(b, E(p)->present); }
/* the MIDI thread: notes on THRU IN go out to the box's port on its channel */
static void ex_midi_in(part_t *p, int port, int st, int d1, int d2)
{
    (void)port;
    ext_t *e = E(p);
    if (!e->thru_in) return;
    int ty = st & 0xF0, ch = (st & 15) + 1;
    if (ch != e->thru_in) return;
    if (ty != 0x80 && ty != 0x90 && ty != 0xB0 && ty != 0xE0) return;
    cm_midi_port(e->dev, -1, ty | (e->thru_ch - 1), d1, d2);
}
void ext_set_present(int k, int on) { if (k >= 0 && k < 2 && X[k].present != on) { X[k].present = on; cm_dirty(); } }
const char *ext_dev(int k) { return k >= 0 && k < 2 ? X[k].dev : ""; }
int ext_thru_in(int k) { return k >= 0 && k < 2 ? X[k].thru_in : 0; }
int ext_audio(int k) { return k >= 0 && k < 2 ? X[k].audio : 0; }
int ext_midi_in(int k) { return k >= 0 && k < 2 ? X[k].midi_in : 1; }
int ext_start(int k) { return k >= 0 && k < 2 ? X[k].start : 1; }
const char *ext_adev(int k) { return k >= 0 && k < 2 ? X[k].adev : ""; }
int ext_ach(int k) { return k >= 0 && k < 2 ? X[k].ach : 0; }

const part_def PART_EXT = {
    .kind = "ext", .name = "EXT", .init = ex_init, .render = ex_render, .state = ex_state, .set = ex_set,
    .save = ex_save, .load = ex_load, .live = ex_live, .midi_in = ex_midi_in,
};
