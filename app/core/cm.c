// SPDX-License-Identifier: GPL-3.0-only
// Chordially core: the clock and transport, the parts (cm_int.h), the event scheduler, MIDI in (keys, controllers
// with MIDI learn, clock / transport / program change) and out (clock to every port that has it on), the delay and
// reverb, the master, the page's messages, the state and live JSON, saving and loading.
// Threads: the audio thread runs cm_render under the lock; the page's messages (main thread) and MIDI input (MIDI
// thread) take the same lock briefly. MIDI input is queued and applied at the start of the next audio block.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cm.h"
#include "cm_int.h"

extern const part_def PART_CHORD, PART_EXT;
int chord_inch(void);
void ext_set_present(int k, int on);
const char *ext_dev(int k);
int ext_thru_in(int k);
int ext_audio(int k); int ext_midi_in(int k); int ext_start(int k); const char *ext_adev(int k); int ext_ach(int k);
static char ADEVS[4096] = "[]";   /* the audio input devices (the shell's list, for the strips' device menus) */

cm_clock_t CK;
part_t PART[3];
int NPART = 3;
int (*cm_ain)(int slot, int n, float *L, float *R);
int (*cm_insert)(float *L, float *R, int n);
int (*cm_instr)(const int *ev, int nev, float *L, float *R, int n);

static pthread_mutex_t LK = PTHREAD_MUTEX_INITIALIZER;
static cm_midi_fn midi_out;
static int in_render;
static volatile int dirty = 1;

/* ------------------------------------------------------------------ global settings */
static struct {
    int vol, dly_time, dly_fb, rev_size, rev_tone, clk_src, tr_in, pc_in;
    int comp, cthr, crat, catk, crel, cmak;   /* the master compressor (0..127 each, see comp_run) */
    int skin;                                 /* the page's skin (SETUP > SKIN) */
} G = {100, 3, 50, 70, 70, 0, 1, 1, 0, 95, 50, 64, 64, 0, 0};
static const double DLY_BEATS[] = {0.25, 1.0 / 3, 0.5, 0.75, 1.0, 1.5, 2.0};
static const char *const DLY_NAME[] = {"1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
#define NDLY 7

/* ------------------------------------------------------------------ ports */
typedef struct { int uid; char name[96]; int clock; } port_t;
static port_t OUTS[32], INS[32]; static int NOUT, NIN;
typedef struct { char name[96]; int on; } clk_t;      /* the clock setting by port name (kept while unplugged) */
static clk_t CLK[48]; static int NCLK;
static int clock_on(const char *name)
{
    for (int i = 0; i < NCLK; i++) if (!strcmp(CLK[i].name, name)) return CLK[i].on;
    return !strstr(name, "IAC");                           /* new ports: on, except the IAC bus */
}
static void clock_set(const char *name, int on)
{
    for (int i = 0; i < NCLK; i++) if (!strcmp(CLK[i].name, name)) { CLK[i].on = on; return; }
    if (NCLK < 48) { snprintf(CLK[NCLK].name, sizeof CLK[NCLK].name, "%s", name); CLK[NCLK++].on = on; }
}
static int ci_contains(const char *h, const char *n)
{
    size_t ln = strlen(n);
    if (!ln) return 0;
    for (const char *p = h; *p; p++) {
        size_t i = 0;
        while (i < ln && p[i] && (p[i] | 32) == (n[i] | 32)) i++;
        if (i == ln) return 1;
    }
    return 0;
}
/* which EXT box a MIDI port belongs to: its name contains that box's name and not the other's ("S-1" yes;
   an aggregate "S-1 and T-8" belongs to neither); -1 = none */
static int port_ext(const char *pname)
{
    int a = ci_contains(pname, ext_dev(0)), b = ci_contains(pname, ext_dev(1));
    return a && !b ? 0 : b && !a ? 1 : -1;
}
static const char *in_name(int uid) { for (int i = 0; i < NIN; i++) if (INS[i].uid == uid) return INS[i].name; return ""; }
static int port_find(const char *name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < NOUT; i++) if (!strcmp(OUTS[i].name, name)) return i;
    for (int i = 0; i < NOUT; i++) if (ci_contains(OUTS[i].name, name)) return i;
    return -1;
}
int cm_port_exists(const char *name, int out)
{
    if (out) return port_find(name) >= 0;
    for (int i = 0; i < NIN; i++) if (ci_contains(INS[i].name, name)) return 1;
    return 0;
}
void cm_midi_port(const char *name, int frame, int st, int d1, int d2)
{
    int i = port_find(name);
    if (i < 0 || !midi_out) return;
    midi_out(OUTS[i].uid, in_render ? frame : -1, st, d1, d2);
}
void cm_set_ports(const char *json)
{
    js_t *m = js_parse(json);
    if (!m) return;
    pthread_mutex_lock(&LK);
    const js_t *a = js_get(m, "outs"), *b = js_get(m, "ins");
    NOUT = 0; NIN = 0;
    if (a) for (const js_t *k = a->kid; k && NOUT < 32; k = k->next) {
        const char *nm = js_strv(k, "name"); OUTS[NOUT].uid = (int)js_num(k, "id", 0);
        snprintf(OUTS[NOUT].name, sizeof OUTS[NOUT].name, "%s", nm ? nm : "?"); NOUT++;
    }
    if (b) for (const js_t *k = b->kid; k && NIN < 32; k = k->next) {
        const char *nm = js_strv(k, "name"); INS[NIN].uid = (int)js_num(k, "id", 0);
        snprintf(INS[NIN].name, sizeof INS[NIN].name, "%s", nm ? nm : "?"); NIN++;
    }
    dirty = 1;
    pthread_mutex_unlock(&LK);
    js_free(m);
}
void cm_ports_json(jb_t *b)
{
    jb_raw(b, "{\"outs\":[");
    for (int i = 0; i < NOUT; i++) { if (i) jb_ch(b, ','); jb_ch(b, '['); jb_str_utf8(b, OUTS[i].name); jb_ch(b, ','); jb_int(b, clock_on(OUTS[i].name)); jb_ch(b, ']'); }
    jb_raw(b, "],\"ins\":[");
    for (int i = 0; i < NIN; i++) { if (i) jb_ch(b, ','); jb_str_utf8(b, INS[i].name); }
    jb_raw(b, "]}");
}

/* ------------------------------------------------------------------ the scheduler */
typedef struct { long long at; part_t *p; int a, b, c; } ev_t;
static ev_t EV[2048]; static int NEV;
void cm_sched(long long at, part_t *p, int a, int b, int c)
{
    if (NEV >= 2048) return;
    int i = NEV++;
    while (i > 0 && EV[i - 1].at > at) { EV[i] = EV[i - 1]; i--; }
    EV[i] = (ev_t){at, p, a, b, c};
}
void cm_unsched(part_t *p)
{
    int k = 0;
    for (int i = 0; i < NEV; i++) if (EV[i].p != p) EV[k++] = EV[i];
    NEV = k;
}
void cm_dirty(void) { dirty = 1; }
void cm_set_bpm(double bpm)
{
    if (bpm < 40) bpm = 40; if (bpm > 300) bpm = 300;
    if (!CK.ext) { CK.bpm = bpm; CK.spt = SR * 60.0 / (bpm * PPQN); }
}

/* ------------------------------------------------------------------ MIDI in: the queue, controllers, learn */
typedef struct { int port, st, d1, d2; } min_t;
static min_t MQ[512]; static int MQN;
typedef struct { int cc; char target[16]; } ccmap_t;
static ccmap_t CC[32]; static int NCC;
static char learn[16];
static void cc_defaults(void)
{
    NCC = 0;   /* the KeyStep 37's four knobs (bank 1, factory CCs 74, 71, 76, 77) */
    const char *t[4] = {"cut", "style", "var", "bpm"}; int c[4] = {74, 71, 76, 77};
    for (int i = 0; i < 4; i++) { CC[NCC].cc = c[i]; snprintf(CC[NCC].target, sizeof CC[NCC].target, "%s", t[i]); NCC++; }
}
static double ext_last_f8 = -1e9, ext_int, ext_phase; static long ext_pending;
static long long samples;   /* the audio clock (absolute samples) */
static int start_req, stop_req, cont_req;

void cm_midi_in(int port, int st, int d1, int d2)
{
    pthread_mutex_lock(&LK);
    if (MQN < 512) MQ[MQN++] = (min_t){port, st, d1, d2};
    for (int k = 1; k < NPART; k++) if (PART[k].d->midi_in) PART[k].d->midi_in(&PART[k], port, st, d2 >= 0 ? d1 : d1, d2);
    pthread_mutex_unlock(&LK);
}
static void apply_cc_target(const char *t, int v)
{
    part_t *c = &PART[0];
    char buf[96];
    int val = -1, have = 0; const char *f = t;
    if (!strcmp(t, "cut") || !strcmp(t, "res") || !strcmp(t, "sub") || !strcmp(t, "dly") || !strcmp(t, "rev") || !strcmp(t, "vol")) val = v;
    else if (!strcmp(t, "style")) val = v * 9 / 128;
    else if (!strcmp(t, "var")) val = v * 8 / 128;
    else if (!strcmp(t, "gate")) val = 10 + v * 90 / 127;
    else if (!strcmp(t, "swing")) val = v * 100 / 127;
    else if (!strcmp(t, "grnd")) val = v * 100 / 127;
    else if (!strcmp(t, "ea") || !strcmp(t, "ed") || !strcmp(t, "es") || !strcmp(t, "er") || !strcmp(t, "eg") || !strcmp(t, "eql") || !strcmp(t, "eqm") || !strcmp(t, "eqh") || !strcmp(t, "pan")) val = v;
    else if (!strcmp(t, "bpm")) { val = (60 + v) * 10; f = "bpm10"; }
    else if (!strcmp(t, "hold")) { val = v >= 64; have = 1; }
    else if (!strcmp(t, "set")) val = v * 64 / 128;
    else if (!strcmp(t, "sound")) val = v * 40 / 128;
    else if (!strncmp(t, "s1", 2) || !strncmp(t, "t8", 2)) {
        part_t *e = &PART[t[0] == 's' ? 1 : 2];
        snprintf(buf, sizeof buf, "{\"t\":\"set\",\"f\":\"%s\",\"v\":%d}", t + 3, v);
        js_t *m = js_parse(buf); e->d->set(e, "set", m); js_free(m); return;
    }
    else {   /* the shared settings (delay, reverb, master, compressor) */
        struct { const char *k; int *p; int max; } GT[] = {{"mvol", &G.vol, 127}, {"dly_fb", &G.dly_fb, 127}, {"rev_size", &G.rev_size, 127},
            {"rev_tone", &G.rev_tone, 127}, {"dly_time", &G.dly_time, NDLY - 1}, {"cthr", &G.cthr, 127}, {"crat", &G.crat, 127},
            {"catk", &G.catk, 127}, {"crel", &G.crel, 127}, {"cmak", &G.cmak, 127}};
        for (unsigned i = 0; i < sizeof GT / sizeof GT[0]; i++) if (!strcmp(t, GT[i].k)) { *GT[i].p = v * (GT[i].max + 1) / 128; dirty = 1; return; }
        if (!strcmp(t, "strum")) val = v * 80 / 127;
        else if (!strcmp(t, "oct")) { val = v * 5 / 128 - 2; have = 1; }
        else if (!strcmp(t, "center")) val = 55 + v * 21 / 127;
    }
    if (val < 0 && !have) return;
    snprintf(buf, sizeof buf, "{\"t\":\"set\",\"f\":\"%s\",\"v\":%d}", f, val);
    js_t *m = js_parse(buf); c->d->set(c, "set", m); js_free(m);
}
static void midi_apply(const min_t *m)
{
    int st = m->st, ty = st & 0xF0, ch = (st & 15) + 1;
    { int k = port_ext(in_name(m->port)); if (k >= 0 && !ext_midi_in(k)) return; }   /* MIDI IN off: nothing from that box (keys, its PLAY / STOP, its clock) */
    if (st == 0xF8) {
        if (G.clk_src != 1) return;
        double t = (double)samples;
        if (t - ext_last_f8 < SR * 0.5) {
            double d = t - ext_last_f8;
            ext_int = ext_int > 0 ? ext_int + (d - ext_int) * 0.08 : d;
        }
        ext_last_f8 = t; ext_pending += PPQN / 24;
        return;
    }
    if (st == 0xFA) { if (G.tr_in) { start_req = 1; ext_pending = 0; ext_phase = 0; } return; }
    if (st == 0xFB) { if (G.tr_in) cont_req = 1; return; }
    if (st == 0xFC) { if (G.tr_in) stop_req = 1; return; }
    if (st >= 0xF0) return;
    if (ty == 0xC0) {
        if (G.pc_in) { char b[64]; snprintf(b, sizeof b, "{\"t\":\"pattern\",\"v\":%d}", m->d1 & 63); js_t *j = js_parse(b); PART[0].d->set(&PART[0], "pattern", j); js_free(j); }
        return;
    }
    /* channels that an EXT part passes through are not the chord part's (unless it asks for that channel) */
    int inch = chord_inch();
    int thru = 0; for (int k = 0; k < 2; k++) if (ext_thru_in(k) == ch) thru = 1;
    int mine = inch ? ch == inch : !thru;
    if (ty == 0xB0) {
        if (learn[0]) {
            for (int i = 0; i < NCC; i++) if (CC[i].cc == m->d1 || !strcmp(CC[i].target, learn)) { memmove(CC + i, CC + i + 1, sizeof(ccmap_t) * (size_t)(NCC - i - 1)); NCC--; i--; }
            if (NCC < 32) { CC[NCC].cc = m->d1; snprintf(CC[NCC].target, sizeof CC[NCC].target, "%s", learn); NCC++; }
            learn[0] = 0; dirty = 1; return;
        }
        for (int i = 0; i < NCC; i++) if (CC[i].cc == m->d1) { apply_cc_target(CC[i].target, m->d2); return; }
        if (mine && PART[0].d->cc) PART[0].d->cc(&PART[0], 0, m->d1, m->d2);
        return;
    }
    if (!mine) return;
    if (ty == 0x90 && m->d2 > 0) PART[0].d->key(&PART[0], 0, m->d1, m->d2);
    else if (ty == 0x80 || ty == 0x90) PART[0].d->key(&PART[0], 0, m->d1, 0);
    else if (ty == 0xE0 && PART[0].d->cc) PART[0].d->cc(&PART[0], 0, -1, m->d1 | (m->d2 << 7));
}

/* ------------------------------------------------------------------ delay + reverb */
#define DMAX 131072
static float dbuf[2][DMAX]; static int dw; static float dlp[2], dtime_s;
static const int RLEN[8] = {1693, 1759, 1621, 1543, 1399, 1471, 1291, 1213};
static float *rbuf[8]; static int rpos[8]; static float rlp[8];
static void fx_init(void)
{
    for (int i = 0; i < 8; i++) { int len = RLEN[i] * 2; rbuf[i] = calloc((size_t)len, sizeof(float)); rpos[i] = 0; }
}
static void fx_run(float *L, float *R, const float *sd, const float *sr, int n)
{
    /* delay: tempo-synced, ping-pong, darker each repeat */
    double want = DLY_BEATS[G.dly_time] * 60.0 / CK.bpm * SR;
    if (want > DMAX - 4) want = DMAX - 4;
    float fb = G.dly_fb / 127.f * 0.85f, lpk = 0.35f;
    /* reverb: 8-line FDN (Householder), decay from SIZE, damping from TONE */
    double rt = 0.4 + pow(G.rev_size / 127.0, 2) * 11.6;
    float g[8]; for (int i = 0; i < 8; i++) g[i] = (float)pow(10.0, -3.0 * RLEN[i] * 2 / (rt * SR));
    float damp = 0.15f + 0.8f * (1.f - G.rev_tone / 127.f);
    for (int i = 0; i < n; i++) {
        dtime_s += (float)((want - dtime_s) * 0.0005);
        float rp = (float)dw - dtime_s; if (rp < 0) rp += DMAX;
        int i0 = (int)rp; float fr = rp - (float)i0;
        float yl = dbuf[0][i0 & (DMAX - 1)] * (1 - fr) + dbuf[0][(i0 + 1) & (DMAX - 1)] * fr;
        float yr = dbuf[1][i0 & (DMAX - 1)] * (1 - fr) + dbuf[1][(i0 + 1) & (DMAX - 1)] * fr;
        dlp[0] += (yl - dlp[0]) * (1 - lpk * 0.6f); dlp[1] += (yr - dlp[1]) * (1 - lpk * 0.6f);
        dbuf[0][dw] = sd[i] + dlp[1] * fb;               /* (left takes the input, right feeds back into left: ping-pong) */
        dbuf[1][dw] = dlp[0] * fb;
        dw = (dw + 1) & (DMAX - 1);
        float ol = yl * 0.8f, or = yr * 0.8f;
        /* reverb */
        float x = sr[i] + 0.15f * (yl + yr), o[8], s = 0;
        for (int k = 0; k < 8; k++) { int len = RLEN[k] * 2; o[k] = rbuf[k][rpos[k]]; rlp[k] += (o[k] - rlp[k]) * (1 - damp); o[k] = rlp[k]; s += o[k]; (void)len; }
        s *= 0.25f;
        float vl = 0, vr = 0;
        for (int k = 0; k < 8; k++) {
            int len = RLEN[k] * 2;
            float v = (o[k] - s) * g[k] + x * 0.35f;
            if (fabsf(v) < 1e-20f) v = 0;
            rbuf[k][rpos[k]] = v; rpos[k] = (rpos[k] + 1) % len;
            if (k & 1) vr += o[k]; else vl += o[k];
        }
        L[i] += ol + vl * 0.5f; R[i] += or + vr * 0.5f;
    }
}

/* ------------------------------------------------------------------ the master compressor
   Stereo-linked, feed-forward, peak detector, 6 dB soft knee, gain smoothed in dB (ATTACK / RELEASE), MAKEUP after.
   THRESH -40..0 dB, RATIO 1..20 (curved, 4:1 near the middle), ATTACK 0.1..100 ms, RELEASE 20..1000 ms, MAKEUP 0..24 dB. */
static double comp_env;     /* the gain reduction now (dB, >= 0) */
static float comp_gr;       /* the most reduction in the last 1/60 s (for the meter) */
static void comp_run(float *L, float *R, int n)
{
    if (!G.comp) { comp_env = 0; return; }
    double thr = -40.0 + G.cthr * 40.0 / 127, rat = 1 + pow(G.crat / 127.0, 2) * 19, knee = 6;
    double atk = 0.1 * pow(1000.0, G.catk / 127.0) * 0.001, rel = 20 * pow(50.0, G.crel / 127.0) * 0.001;
    double ka = exp(-1.0 / (atk * SR)), kr = exp(-1.0 / (rel * SR)), mk = pow(10.0, G.cmak * 24.0 / 127 / 20);
    for (int i = 0; i < n; i++) {
        double x = fabs(L[i]) > fabs(R[i]) ? fabs(L[i]) : fabs(R[i]);
        double xd = x > 1e-6 ? 20 * log10(x) : -120, over = xd - thr, gr;
        if (over <= -knee / 2) gr = 0;
        else if (over >= knee / 2) gr = over * (1 - 1 / rat);
        else { double t = over + knee / 2; gr = (1 - 1 / rat) * t * t / (2 * knee); }
        comp_env = gr > comp_env ? ka * comp_env + (1 - ka) * gr : kr * comp_env + (1 - kr) * gr;
        double g = pow(10.0, -comp_env / 20) * mk;
        L[i] = (float)(L[i] * g); R[i] = (float)(R[i] * g);
        if (comp_env > comp_gr) comp_gr = (float)comp_env;
    }
}

/* ------------------------------------------------------------------ clock out */
static void clock_out(int frame, int st)
{
    if (!midi_out) return;
    for (int i = 0; i < NOUT; i++) if (clock_on(OUTS[i].name)) {
        { int k = port_ext(OUTS[i].name); if (k >= 0 && !ext_start(k)) continue; }   /* SYNC off: the box gets no clock and no start / stop at all (Roland boxes may run on clock alone) */
        midi_out(OUTS[i].uid, frame, st, 0, 0);
    }
}

/* ------------------------------------------------------------------ init */
void cm_init(void)
{
    memset(&CK, 0, sizeof CK);
    CK.bpm = 120; CK.spt = SR * 60.0 / (120 * PPQN);
    PART[0].d = &PART_CHORD; PART[0].idx = 0;
    PART[1].d = &PART_EXT; PART[1].idx = 1;
    PART[2].d = &PART_EXT; PART[2].idx = 2;
    for (int k = 0; k < NPART; k++) PART[k].d->init(&PART[k]);
    cc_defaults();
    fx_init();
}
void cm_set_midi_out(cm_midi_fn f) { midi_out = f; }
void cm_set_ain(int (*f)(int, int, float *, float *)) { cm_ain = f; }
void cm_set_insert(int (*f)(float *, float *, int)) { cm_insert = f; }
void cm_set_instr(int (*f)(const int *, int, float *, float *, int)) { cm_instr = f; }
void cm_transport(double *bpm, int *playing, double *beat) { *bpm = CK.bpm; *playing = CK.playing; *beat = CK.playing ? (double)CK.ptick / PPQN : 0; }

/* ------------------------------------------------------------------ the audio block */
static double tick_acc;      /* samples until the next internal tick */
static float mpk;
static void do_tick(int frame)
{
    if (CK.playing && (CK.ptick % (PPQN / 24)) == 0 && G.clk_src == 0) clock_out(frame, 0xF8);
    else if (!CK.playing && (CK.tick % (PPQN / 24)) == 0 && G.clk_src == 0) clock_out(frame, 0xF8);
    for (int k = 0; k < NPART; k++) if (PART[k].d->tick) PART[k].d->tick(&PART[k], frame, CK.tick);
    CK.tick++;
    if (CK.playing) CK.ptick++;
}
static void block(float *L, float *R, int n)
{
    CK.now = samples;
    /* MIDI in, queued since the last block */
    for (int i = 0; i < MQN; i++) midi_apply(&MQ[i]);
    MQN = 0;
    CK.ext = G.clk_src == 1 && samples - ext_last_f8 < SR * 0.5 && ext_int > 0;
    if (CK.ext) { CK.bpm = SR * 60.0 / (ext_int * 24); CK.spt = ext_int / (PPQN / 24); }
    if (stop_req) {
        stop_req = 0;
        if (CK.playing) {
            CK.playing = 0;
            if (G.clk_src == 0) clock_out(0, 0xFC);
            for (int k = 0; k < NPART; k++) if (PART[k].d->transport) PART[k].d->transport(&PART[k], 0, 0);
            dirty = 1;
        }
    }
    if (start_req || cont_req) {
        int cont = cont_req && !start_req;
        start_req = cont_req = 0;
        if (!CK.playing || !cont) {
            if (CK.playing) for (int k = 0; k < NPART; k++) if (PART[k].d->transport) PART[k].d->transport(&PART[k], 0, 0);
            CK.playing = 1; if (!cont) CK.ptick = 0;
            if (G.clk_src == 0) clock_out(0, cont ? 0xFB : 0xFA);
            for (int k = 0; k < NPART; k++) if (PART[k].d->transport) PART[k].d->transport(&PART[k], 0, 1);
            tick_acc = 0; ext_phase = 0;
            dirty = 1;
        }
    }
    /* the ticks in this block */
    if (CK.ext) {
        if (ext_pending > 6) { while (ext_pending > 4) { do_tick(0); ext_pending--; } }
        double step = CK.spt;
        for (int f = 0; f < n; f++) {
            if (ext_pending > 0 && ext_phase <= 0) { do_tick(f); ext_pending--; ext_phase += step * (ext_pending > 2 ? 0.8 : 1.0); }
            ext_phase -= 1;
            if (ext_phase < -step) ext_phase = 0;
        }
    } else {
        ext_pending = 0;
        for (int f = 0; f < n; f++) {
            if (tick_acc <= 0) { do_tick(f); tick_acc += CK.spt; }
            tick_acc -= 1;
        }
    }
    /* the scheduled events due in this block */
    int k = 0;
    while (k < NEV && EV[k].at < samples + n) {
        ev_t e = EV[k++];
        int f = (int)(e.at - samples); if (f < 0) f = 0;
        e.p->d->event(e.p, f, e.a, e.b, e.c);
    }
    if (k) { memmove(EV, EV + k, sizeof(ev_t) * (size_t)(NEV - k)); NEV -= k; }
    /* render: the parts, then the effects, the master */
    float sd[MAXF], sr[MAXF];
    memset(L, 0, sizeof(float) * (size_t)n); memset(R, 0, sizeof(float) * (size_t)n);
    memset(sd, 0, sizeof(float) * (size_t)n); memset(sr, 0, sizeof(float) * (size_t)n);
    for (int p = 0; p < NPART; p++) if (PART[p].d->render) PART[p].d->render(&PART[p], L, R, sd, sr, n);
    fx_run(L, R, sd, sr, n);
    comp_run(L, R, n);
    float g = G.vol / 100.f; g *= g;
    float pk = 0;
    for (int i = 0; i < n; i++) {
        float a = L[i] * g, b = R[i] * g;
        /* the master: linear to 0.7, then a soft knee to -1 dBFS */
        a = a > 0.7f ? 0.7f + 0.19f * tanhf((a - 0.7f) / 0.19f) : a < -0.7f ? -0.7f - 0.19f * tanhf((-a - 0.7f) / 0.19f) : a;
        b = b > 0.7f ? 0.7f + 0.19f * tanhf((b - 0.7f) / 0.19f) : b < -0.7f ? -0.7f - 0.19f * tanhf((-b - 0.7f) / 0.19f) : b;
        if (!(a == a)) a = 0; if (!(b == b)) b = 0;
        L[i] = a; R[i] = b;
        float x = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b); if (x > pk) pk = x;
    }
    mpk = pk > mpk ? pk : mpk * 0.92f;
    samples += n;
}
void cm_render(float *L, float *R, int n)
{
    pthread_mutex_lock(&LK);
    in_render = 1;
    int done = 0;
    while (done < n) { int m = n - done > 128 ? 128 : n - done; block(L + done, R + done, m); done += m; }
    in_render = 0;
    pthread_mutex_unlock(&LK);
}
long long cm_samples(void) { return samples; }
long long cm_block_now(void) { return CK.now; }
int cm_block_frame_base(void) { return 0; }

/* ------------------------------------------------------------------ the page */
static void global_json(jb_t *b)
{
    jb_raw(b, "{");
#define F(k) jb_key(b, #k); jb_int(b, G.k);
    F(vol) F(dly_time) F(dly_fb) F(rev_size) F(rev_tone) F(clk_src) F(tr_in) F(pc_in) F(comp) F(cthr) F(crat) F(catk) F(crel) F(cmak) F(skin)
#undef F
    jb_key(b, "dly_names"); jb_ch(b, '['); for (int i = 0; i < NDLY; i++) { if (i) jb_ch(b, ','); jb_str_utf8(b, DLY_NAME[i]); } jb_ch(b, ']');
    jb_raw(b, "}");
}
static char *state_json(void)
{
    jb_t b; jb_init(&b);
    jb_raw(&b, "{\"t\":\"state\",\"g\":"); global_json(&b);
    jb_raw(&b, ",\"ports\":"); cm_ports_json(&b);
    jb_raw(&b, ",\"adevs\":"); jb_raw(&b, ADEVS);
    jb_raw(&b, ",\"cc\":[");
    for (int i = 0; i < NCC; i++) { if (i) jb_ch(&b, ','); jb_ch(&b, '['); jb_int(&b, CC[i].cc); jb_ch(&b, ','); jb_str_utf8(&b, CC[i].target); jb_ch(&b, ']'); }
    jb_raw(&b, "],\"learn\":"); jb_str_utf8(&b, learn);
    jb_raw(&b, ",\"chord\":{"); PART[0].d->state(&PART[0], &b); jb_ch(&b, '}');
    jb_raw(&b, ",\"ext\":[");
    for (int k = 1; k < NPART; k++) { if (k > 1) jb_ch(&b, ','); jb_ch(&b, '{'); PART[k].d->state(&PART[k], &b); jb_ch(&b, '}'); }
    jb_raw(&b, "]}");
    return b.b;
}
char *cm_state(int force)
{
    pthread_mutex_lock(&LK);
    char *s = NULL;
    if (dirty || force) { dirty = 0; s = state_json(); }
    pthread_mutex_unlock(&LK);
    return s;
}
char *cm_live(void)
{
    pthread_mutex_lock(&LK);
    jb_t b; jb_init(&b);
    jb_raw(&b, "{\"t\":\"live\",\"play\":"); jb_int(&b, CK.playing);
    jb_raw(&b, ",\"bpm\":"); jb_num(&b, floor(CK.bpm * 10 + 0.5) / 10);
    jb_raw(&b, ",\"ext\":"); jb_int(&b, CK.ext);
    jb_raw(&b, ",\"beat\":"); jb_num(&b, CK.playing ? (double)CK.ptick / PPQN : 0);
    jb_raw(&b, ",\"pk\":[");
    for (int k = 0; k < NPART; k++) { jb_num(&b, floor(PART[k].peak * 1000) / 1000); jb_ch(&b, ','); }
    jb_num(&b, floor(mpk * 1000) / 1000); jb_ch(&b, ']');
    jb_raw(&b, ",\"gr\":"); jb_num(&b, floor(comp_gr * 10) / 10); comp_gr = 0;
    jb_raw(&b, ",\"c\":{"); PART[0].d->live(&PART[0], &b); jb_ch(&b, '}');
    jb_raw(&b, ",\"x\":[");
    for (int k = 1; k < NPART; k++) { if (k > 1) jb_ch(&b, ','); jb_ch(&b, '{'); PART[k].d->live(&PART[k], &b); jb_ch(&b, '}'); }
    jb_raw(&b, "]}");
    pthread_mutex_unlock(&LK);
    return b.b;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void cm_msg(const char *json)
{
    js_t *m = js_parse(json);
    if (!m) return;
    const char *t = js_strv(m, "t");
    if (!t) { js_free(m); return; }
    pthread_mutex_lock(&LK);
    int v = (int)js_num(m, "v", 0);
    if (!strcmp(t, "hello")) dirty = 1;
    else if (!strcmp(t, "play")) start_req = 1;
    else if (!strcmp(t, "stop")) { if (CK.playing) stop_req = 1; else { const char *pn = "panic"; PART[0].d->set(&PART[0], pn, m); } }
    else if (!strcmp(t, "key")) { if (MQN < 512) MQ[MQN++] = (min_t){-2, v > 0 ? 0x90 : 0x80, (int)js_num(m, "n", 60), v}; }
    else if (!strcmp(t, "g")) {
        const char *f = js_strv(m, "f");
        if (f) {
#define S(k, lo, hi) if (!strcmp(f, #k)) G.k = clampi(v, lo, hi);
            S(vol, 0, 127) S(dly_time, 0, NDLY - 1) S(dly_fb, 0, 127) S(rev_size, 0, 127) S(rev_tone, 0, 127)
            S(clk_src, 0, 1) S(tr_in, 0, 1) S(pc_in, 0, 1) S(comp, 0, 1) S(cthr, 0, 127) S(crat, 0, 127) S(catk, 0, 127) S(crel, 0, 127) S(cmak, 0, 127) S(skin, 0, 5)
#undef S
            dirty = 1;
        }
    }
    else if (!strcmp(t, "clockport")) { const char *s = js_strv(m, "s"); if (s) { clock_set(s, v); dirty = 1; } }
    else if (!strcmp(t, "learn")) { const char *s = js_strv(m, "s"); snprintf(learn, sizeof learn, "%s", s ? s : ""); dirty = 1; }
    else if (!strcmp(t, "unlearn")) {
        const char *s = js_strv(m, "s");
        for (int i = 0; s && i < NCC; i++) if (!strcmp(CC[i].target, s)) { memmove(CC + i, CC + i + 1, sizeof(ccmap_t) * (size_t)(NCC - i - 1)); NCC--; i--; }
        dirty = 1;
    }
    else if (!strcmp(t, "ccdefault")) { cc_defaults(); dirty = 1; }
    else if (!strcmp(t, "ext")) { int k = clampi((int)js_num(m, "k", 0), 0, 1) + 1; PART[k].d->set(&PART[k], "set", m); }
    else PART[0].d->set(&PART[0], t, m);
    pthread_mutex_unlock(&LK);
    js_free(m);
}

/* ------------------------------------------------------------------ the shell's questions */
int cm_ext_count(void) { return 2; }
const char *cm_ext_dev(int k) { return ext_dev(k); }
int cm_ext_audio(int k) { return ext_audio(k); }
const char *cm_ext_adev(int k) { return ext_adev(k); }
int cm_ext_ach(int k) { return ext_ach(k); }
void cm_set_adevs(const char *json) { pthread_mutex_lock(&LK); if (strcmp(ADEVS, json)) { snprintf(ADEVS, sizeof ADEVS, "%s", json); dirty = 1; } pthread_mutex_unlock(&LK); }
void cm_ext_present(int k, int on) { pthread_mutex_lock(&LK); ext_set_present(k, on); pthread_mutex_unlock(&LK); }

/* ------------------------------------------------------------------ save / load */
char *cm_save(void)
{
    pthread_mutex_lock(&LK);
    jb_t b; jb_init(&b);
    jb_raw(&b, "{\"app\":\"Chordially\",\"version\":1,\"g\":"); global_json(&b);
    jb_raw(&b, ",\"cc\":[");
    for (int i = 0; i < NCC; i++) { if (i) jb_ch(&b, ','); jb_ch(&b, '['); jb_int(&b, CC[i].cc); jb_ch(&b, ','); jb_str_utf8(&b, CC[i].target); jb_ch(&b, ']'); }
    jb_raw(&b, "],\"clock\":[");
    for (int i = 0; i < NCLK; i++) { if (i) jb_ch(&b, ','); jb_ch(&b, '['); jb_str_utf8(&b, CLK[i].name); jb_ch(&b, ','); jb_int(&b, CLK[i].on); jb_ch(&b, ']'); }
    jb_raw(&b, "],\"parts\":[");
    for (int k = 0; k < NPART; k++) { if (k) jb_ch(&b, ','); jb_ch(&b, '{'); jb_key(&b, "kind"); jb_str_utf8(&b, PART[k].d->kind); PART[k].d->save(&PART[k], &b); jb_ch(&b, '}'); }
    jb_raw(&b, "]}");
    pthread_mutex_unlock(&LK);
    return b.b;
}
int cm_load(const char *json)
{
    js_t *m = js_parse(json);
    if (!m) return -1;
    pthread_mutex_lock(&LK);
    const js_t *g = js_get(m, "g");
    if (g) {
#define L(k, lo, hi) G.k = clampi((int)js_num(g, #k, G.k), lo, hi);
        L(vol, 0, 127) L(dly_time, 0, NDLY - 1) L(dly_fb, 0, 127) L(rev_size, 0, 127) L(rev_tone, 0, 127) L(clk_src, 0, 1) L(tr_in, 0, 1) L(pc_in, 0, 1) L(comp, 0, 1) L(cthr, 0, 127) L(crat, 0, 127) L(catk, 0, 127) L(crel, 0, 127) L(cmak, 0, 127) L(skin, 0, 5)
#undef L
    }
    const js_t *cc = js_get(m, "cc");
    if (cc && cc->type == JS_ARR) {
        NCC = 0;
        for (const js_t *e = cc->kid; e && NCC < 32; e = e->next) {
            const js_t *a = js_at(e, 0), *b2 = js_at(e, 1);
            if (a && b2 && b2->type == JS_STR) { CC[NCC].cc = (int)a->num; snprintf(CC[NCC].target, sizeof CC[NCC].target, "%s", b2->str); NCC++; }
        }
    }
    const js_t *ck = js_get(m, "clock");
    if (ck && ck->type == JS_ARR) for (const js_t *e = ck->kid; e; e = e->next) {
        const js_t *a = js_at(e, 0), *b2 = js_at(e, 1);
        if (a && a->type == JS_STR && b2) clock_set(a->str, (int)b2->num);
    }
    const js_t *ps = js_get(m, "parts");
    if (ps && ps->type == JS_ARR) { int k = 0; for (const js_t *e = ps->kid; e && k < NPART; e = e->next, k++) PART[k].d->load(&PART[k], e); }
    dirty = 1;
    pthread_mutex_unlock(&LK);
    js_free(m);
    return 0;
}
