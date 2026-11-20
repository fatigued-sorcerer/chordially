// SPDX-License-Identifier: GPL-3.0-only
// Jammer classic engines (J2, set 2): shared framework for ORACLE (oracle.c), FATHOM (fathom.c) and POLY ANALOG (polyan.c).
// Everything here is static (each engine file gets its own copy); no globals, all state lives in the instance.
//
// The engine file defines, BEFORE including this header:
//   typedef struct vx { ... } vx_t;   per-voice DSP state (oscillators, filters, ramps)
//   typedef struct gx { ... } gx_t;   per-instance DSP state (FX, buffers)
// and then a va_spec (parameter table, pages, presets, macros, mod sources/destinations, DSP callbacks).
//
// Timing model (what makes it sample accurate AND block-split invariant):
//   - MIDI events are queued with their frame; render splits the block at every event frame.
//   - Control-rate work (envelopes, LFOs, mod matrix, parameter smoothing, filter coefficient targets) runs on a
//     GLOBAL grid of VA_CR samples (an instance sample clock), never on block boundaries. A voice that starts between
//     grid points gets one extra short tick up to the next grid point. Per-sample values (amplitude, pitch increment,
//     cutoff) are linearly interpolated across each tick, so nothing zippers.
//   So the same MIDI rendered with any block size gives the same audio (float rounding aside).
//
// Voices: VA_POLY (16) logical voices on VA_NV (20) physical slots. A stolen voice fades out over 3 ms in a spare
// slot while the new note starts in another one, so steals never click. Allocation (the rule): a re-struck note
// never reuses a voice that is ringing in its release; only a key that is still held (or held by the sustain pedal)
// retriggers its own voice. When all 16 are busy: steal the quietest released voice, else the oldest held one.
// MONO: last-note priority, LEGATO (on = no envelope retrigger while a key is held), GLIDE. UNISON = MONO with STACK
// voices on the note (DETUNE spreads them, SPREAD pans them).
#ifndef JAMMER_VA2_COMMON_H
#define JAMMER_VA2_COMMON_H
struct jviz;                 /* file-scope tag before jeng.h uses it in jeng_def_t.viz's parameter list */
#include "../../core/jeng.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define VA_POLY 16
#define VA_NV 20
#define VA_CR 16
#define VA_MAXP 200
#define VA_MAXEV 512
#define VA_MAXDST 48
#define VA_BLOCK 1024
#define VA_PI 3.14159265358979f

/* ------------------------------------------------------------------ DSP primitives */
static inline float va_clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline int va_clampi(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
/* sin(2 pi ph), ph in [0,1): parabolic approximation + refinement (|error| ~ 1e-3) */
static inline float va_sin(float ph) {
    float u = ph * 2.f - 1.f;
    float y = 4.f * u * (1.f - fabsf(u));
    y = 0.225f * (y * fabsf(y) - y) + y;
    return -y;
}
static inline float va_tanh(float x) {               /* Pade, exact saturation at |x| >= 3 */
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
/* 2^x, |rel error| < 2e-4 for |x| < 30 (audio-rate FM) */
static inline float va_exp2(float x) {
    if (x < -30.f) x = -30.f;
    if (x > 30.f) x = 30.f;
    float fl = floorf(x), f = x - fl;
    float p = 1.f + f * (0.6951786f + f * (0.2261527f + f * 0.0780463f));
    union { float f; int32_t i; } u; u.f = p; u.i += (int32_t)fl * (1 << 23);
    return u.f;
}
static inline uint32_t va_rng(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float va_noise(uint32_t *s) { return (float)(int32_t)va_rng(s) * (1.f / 2147483648.f); }
/* polyBLEP / polyBLAMP residuals (t = phase in [0,1), dt = increment) */
static inline float va_blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}
static inline float va_blamp(float t, float dt) {
    if (t < dt) { t = t / dt - 1.f; return -(1.f / 3.f) * t * t * t; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt + 1.f; return (1.f / 3.f) * t * t * t; }
    return 0.f;
}
static inline float va_wrap1(float p) { return p - floorf(p); }
/* band-limited waves */
static inline float va_saw(float p, float dt) { return 2.f * p - 1.f - va_blep(p, dt); }
static inline float va_pulse(float p, float dt, float pw) {          /* DC-free (the 2pw-1 mean removed) */
    float v = p < pw ? 1.f : -1.f;
    float q = p - pw; if (q < 0.f) q += 1.f;
    return v + va_blep(p, dt) - va_blep(q, dt) - (2.f * pw - 1.f);
}
static inline float va_tri(float p, float dt) {                      /* -1 at p = 0, +1 at p = 0.5 */
    float v = 4.f * fabsf(p - 0.5f) - 1.f;
    v = -v;
    float q = p + 0.5f; if (q >= 1.f) q -= 1.f;
    return v + 4.f * dt * (va_blamp(p, dt) - va_blamp(q, dt));
}
/* the naive values (for hard-sync step heights) */
static inline float va_saw_n(float p) { return 2.f * p - 1.f; }
static inline float va_pulse_n(float p, float pw) { return (p < pw ? 1.f : -1.f) - (2.f * pw - 1.f); }
static inline float va_tri_n(float p) { return 1.f - 4.f * fabsf(p - 0.5f); }
static inline float va_mtof(float note) { return 440.f * va_exp2((note - 69.f) * (1.f / 12.f)); }

/* ------------------------------------------------------------------ value mappings (documented in the README) */
/* glide / LFO delay times: t(v) = 1.5 ms * 8000^(v/127): 0 -> 1.5 ms, 64 -> 139 ms, 100 -> 1.71 s, 127 -> 12 s */
static float va_time(float v) { return 0.0015f * powf(8000.f, va_clampf(v, 0.f, 127.f) * (1.f / 127.f)); }
/* envelope times (J2-5 law, same fader feel in every engine): x = v / 127, t = tmin + (tmax - tmin) * x^4
   stage 0 ATTACK 1 ms .. 20 s (1 ms = this envelope's click-free minimum), 1 DECAY 1 ms .. 20 s, 2 RELEASE 2 ms .. 30 s
   (attack: 10 % = 3 ms, 25 % = 79 ms, 50 % = 1.25 s, 75 % = 6.3 s; until J2-5: 1.5 ms * 8000^(v/127) for all three) */
static const float VA_ETMIN[3] = {0.001f, 0.001f, 0.002f}, VA_ETMAX[3] = {20.f, 20.f, 30.f};
static float va_env_time(int st, float v) {
    float x = va_clampf(v, 0.f, 127.f) * (1.f / 127.f), x2 = x * x;
    return VA_ETMIN[st] + (VA_ETMAX[st] - VA_ETMIN[st]) * x2 * x2;
}
/* LFO rate: 0.02 Hz * 2500^(v/127): 0 -> 0.02 Hz, 64 -> 1.03 Hz, 90 -> 5.2 Hz, 127 -> 50 Hz */
static float va_lfohz(float v) { return 0.02f * powf(2500.f, va_clampf(v, 0.f, 127.f) * (1.f / 127.f)); }
/* cutoff: 20 Hz * 1000^(v/127) (20 Hz .. 20 kHz); in semitones above 20 Hz: v * 119.59 / 127 */
static inline float va_cut_st(float v) { return v * (119.589f / 127.f); }
static inline float va_cut_hz(float v) { return 20.f * powf(1000.f, v * (1.f / 127.f)); }
static float va_level_gain(float v) { float x = v * 0.01f; return x * x; }  /* 100 = 0 dB, 127 = +4.2 dB */
/* velocity gain: sens 0 = always 1; vel 100 = 1 (unity at the calibration velocity), 127 -> up to +4.6 dB, 40 -> -12 dB */
static inline float va_velgain(float vel01, float sens) {
    float x = vel01 * 1.27f;
    return 1.f - sens + sens * x * sqrtf(x);
}

static const struct { const char *name; float beats; } VA_SYNC[17] = {
    {"8 BAR", 32.f}, {"4 BAR", 16.f}, {"2 BAR", 8.f}, {"1 BAR", 4.f}, {"1/2 D", 3.f}, {"1/2", 2.f}, {"1/2 T", 4.f / 3.f},
    {"1/4 D", 1.5f}, {"1/4", 1.f}, {"1/4 T", 2.f / 3.f}, {"1/8 D", 0.75f}, {"1/8", 0.5f}, {"1/8 T", 1.f / 3.f},
    {"1/16 D", 0.375f}, {"1/16", 0.25f}, {"1/16 T", 1.f / 6.f}, {"1/32", 0.125f}};
static inline int va_syncidx(int v) { return va_clampi(v * 17 / 128, 0, 16); }

/* ------------------------------------------------------------------ parameter specs */
enum { F_PCT, F_BIP, F_TATK, F_TDEC, F_TREL, F_LFORATE, F_LFODEL, F_SEMI, F_CENT, F_CHOICE, F_TOGGLE, F_LEVEL, F_PAN, F_CUTOFF, F_AMT,
       F_INT, F_GLIDE, F_SRC, F_DST, F_PHASE, F_PW, F_OCT, F_ST, F_VOICES, F_MORPH, F_HZ };
typedef struct { const char *name; int page, min, max, def, fmt, basic; const char *const *ch; int aux; } va_ps;
typedef struct { const char *name; int n; struct { int p, w, pos; } t[4]; } va_macro;   /* pos 1 / -1: acts on the + / - half only */
typedef struct { const char *name, *cat; int flags; const char *arp; const short *kv; } va_preset;

static const char *const VA_LFOW[7] = {"SINE", "TRI", "SAW UP", "SAW DN", "SQUARE", "S&H", "SMOOTH RND"};
static const char *const VA_RETRIG[2] = {"FREE", "KEY"};
static const char *const VA_FREESYNC[2] = {"FREE", "SYNC"};
static const char *const VA_OFFON[2] = {"OFF", "ON"};
static const char *const VA_VOICEMODE[3] = {"POLY", "MONO", "UNISON"};
static const char *const VA_LEGATO[2] = {"RETRIG", "LEGATO"};

#define VA_NCH(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define PK(nm, pg, mn, mx, df, fmt, b) {nm, pg, mn, mx, df, fmt, b, NULL, -1}
#define PC(nm, pg, chs, df, b) {nm, pg, 0, VA_NCH(chs) - 1, df, F_CHOICE, b, chs, -1}
#define PT(nm, pg, df, b) {nm, pg, 0, 1, df, F_TOGGLE, b, VA_OFFON, -1}
/* envelope page: ATTACK DECAY SUSTAIN RELEASE VEL (5 params) */
#define VA_ENV_PS(pg, a, d, s, r, vel, b)                                                                    \
    PK("ATTACK", pg, 0, 127, a, F_TATK, b), PK("DECAY", pg, 0, 127, d, F_TDEC, b),                         \
    PK("SUSTAIN", pg, 0, 127, s, F_PCT, b), PK("RELEASE", pg, 0, 127, r, F_TREL, b), PK("VEL", pg, 0, 127, vel, F_PCT, 0)
/* LFO page: WAVE RATE SYNC DELAY RETRIG PHASE (6 params); base = index of WAVE. SYNC (FREE / SYNC) and RETRIG
   (FREE / KEY) are 2-value switches in the BASIC view of every LFO page */
#define VA_LFO_PS(pg, base, wave, rate, b)                                                                   \
    PC("WAVE", pg, VA_LFOW, wave, b), {"RATE", pg, 0, 127, rate, F_LFORATE, b, NULL, (base) + 2},           \
    PC("SYNC", pg, VA_FREESYNC, 0, 1), PK("DELAY", pg, 0, 127, 0, F_LFODEL, 0), PC("RETRIG", pg, VA_RETRIG, 0, 1), \
    PK("PHASE", pg, 0, 127, 0, F_PHASE, 0)
/* one mod slot: SOURCE DEST AMOUNT (ranges of SOURCE / DEST are set from the engine's lists at create) */
#define VA_MOD_PS(pg) {"SOURCE", pg, 0, 1, 0, F_SRC, 0, NULL, -1}, {"DEST", pg, 0, 1, 0, F_DST, 0, NULL, -1}, \
    PK("AMOUNT", pg, -100, 100, 0, F_AMT, 0)
#define VA_ENV_N 5
#define VA_LFO_N 6
#define VA_MOD_N 24
/* presets: PR("Name", "Cat", flags, arp-json-or-NULL, param, value, param, value, ...) */
#define PR(nm, cat, fl, arp, ...) {nm, cat, fl, arp, (const short[]){__VA_ARGS__, -1}}

/* mod sources (framework ids; each engine lists the ones it offers, in its menu order) */
enum { S_OFF, S_LFO1, S_LFO2, S_LFO3, S_FENV, S_AENV, S_MENV, S_VEL, S_NOTE, S_WHEEL, S_AT, S_RAND, S_BEND, S_STACK,
       S_NSRC };
static const char *const VA_SRCNAME[S_NSRC] = {"OFF", "LFO 1", "LFO 2", "LFO 3", "FILTER ENV", "AMP ENV", "MOD ENV",
                                               "VELOCITY", "NOTE", "MOD WHEEL", "AFTERTOUCH", "RANDOM", "PITCH BEND",
                                               "STACK POS"};

/* ------------------------------------------------------------------ voice + instance */
typedef struct { float v; int st; } va_env;           /* st: 0 off, 1 attack, 2 decay/sustain, 3 release */
enum { V_IDLE, V_HELD, V_REL, V_FADE };

typedef struct va_voice {
    int state, note, sustained, dying, slot, age;
    float vel, rnd, stackpos, pan, sgain;   /* sgain: 1/sqrt(stack size), keeps UNISON at the POLY level */
    float pitch, ptgt;                 /* glide: current and target note (semitones) */
    float det;                         /* stack/voice detune + analog drift (semitones) */
    float pbase;                       /* pitch + bend + det, computed each tick */
    float fade, fstep;                 /* steal fade */
    float vr[6], drift, driftt, dtgt;  /* fixed per-slot randoms (analog voice variation), slow drift */
    uint32_t rng;
    va_env env[3];
    float lph[3], lsh[3], lsh0[3], lfo[3];
    float t;                           /* seconds since note on (LFO delay) */
    float src[S_NSRC];
    float mod[VA_MAXDST];
    vx_t x;
} va_voice;

struct va;
typedef struct va_spec {
    int np; const va_ps *ps;
    int npages; const char *const *pages;
    const va_macro *macros;
    int npre; const va_preset *pre;
    int nsrc; const signed char *src;
    int ndst; const char *const *dst;
    int nenv, nlfo;
    int p_env[3], p_lfo[3], p_mod;
    int p_voices, p_stack, p_detune, p_glide, p_legato, p_bend, p_level, p_pan, p_spread, p_slop, p_cutoff;
    int d_lforate[3], d_lfoamt[3], d_atk, d_dec, d_rel;
    unsigned long long dquad;          /* destinations whose AMOUNT is squared (pitch: fine control near 0) */
    float poly_det;                    /* share of DETUNE applied as a fixed per-voice offset in POLY (classic wide-poly style) */
    void (*init)(struct va *e);
    void (*greset)(struct va *e);
    void (*gtick)(struct va *e);
    void (*vstart)(struct va *e, va_voice *v, int fresh);
    void (*vtick)(struct va *e, va_voice *v, int n);
    void (*vrender)(struct va *e, va_voice *v, float *L, float *R, int n);   /* writes n frames */
    void (*fx)(struct va *e, float *L, float *R, int n);                      /* in place, per chunk; may be NULL */
} va_spec;

typedef struct { int min, max, kind; } va_prange;
typedef struct { int frame, st, d1, d2, seq; } va_ev;

typedef struct va {
    const va_spec *sp;
    float sr, isr;
    va_prange pr[VA_MAXP];
    int val[VA_MAXP], eff[VA_MAXP], add[VA_MAXP], mac[8];
    float ps[VA_MAXP];                 /* smoothed effective values (control rate) */
    int snap;
    va_voice v[VA_NV];
    int agec;
    va_ev ev[VA_MAXEV]; int nev, evseq;
    /* controllers */
    float wheel, at, bend, wheel_s, at_s, bend_s, bend_st;
    int sustain;
    int held[32], heldv[32], nheld;    /* mono key stack */
    int mono[VA_POLY], nmono, mono_note;
    float last_pitch, next_sgain; int have_last;
    unsigned clk;                      /* instance sample clock (control grid) */
    float smc, ccc, bdc;               /* smoothing coefficients per tick */
    /* global LFOs */
    float gph[3], gsh[3], gsh0[3], glfo[3];
    uint32_t grng;
    double bpm; int playing;
    /* output */
    float lev, levd;
    float lim, limA, limR, dcx[2], dcy[2], dcR;
    float tL[VA_CR], tR[VA_CR];
    double pk;
    char tb[8][48]; int ti;
    gx_t g;
} va;

/* ------------------------------------------------------------------ parameters */
static void va_recalc(va *e) {
    const va_spec *sp = e->sp;
    for (int i = 0; i < sp->np; i++) e->add[i] = 0;
    for (int m = 0; m < 8; m++) {
        int mv = e->mac[m]; if (!mv) continue;
        const va_macro *mc = &sp->macros[m];
        for (int j = 0; j < mc->n; j++) {
            int i = mc->t[j].p;
            if ((mc->t[j].pos > 0 && mv < 0) || (mc->t[j].pos < 0 && mv > 0)) continue;
            e->add[i] += mv * (e->pr[i].max - e->pr[i].min) * mc->t[j].w / (127 * 100);
        }
    }
    for (int i = 0; i < sp->np; i++) e->eff[i] = va_clampi(e->val[i] + e->add[i], e->pr[i].min, e->pr[i].max);
}
static int va_okp(va *e, int i) { return i >= 0 && i < e->sp->np; }
static int va_npages(void *p) { return ((va *)p)->sp->npages; }
static const char *va_page_name(void *p, int pg) { va *e = p; return (pg >= 0 && pg < e->sp->npages) ? e->sp->pages[pg] : ""; }
static int va_nparams(void *p) { return ((va *)p)->sp->np; }
static int va_param_page(void *p, int i) { va *e = p; return va_okp(e, i) ? e->sp->ps[i].page : 0; }
static const char *va_param_name(void *p, int i) { va *e = p; return va_okp(e, i) ? e->sp->ps[i].name : ""; }
static int va_param_kind(void *p, int i) { va *e = p; return va_okp(e, i) ? e->pr[i].kind : JP_KNOB; }
static int va_param_min(void *p, int i) { va *e = p; return va_okp(e, i) ? e->pr[i].min : 0; }
static int va_param_max(void *p, int i) { va *e = p; return va_okp(e, i) ? e->pr[i].max : 0; }
static int va_param_basic(void *p, int i) { va *e = p; return va_okp(e, i) ? e->sp->ps[i].basic : 0; }
static int va_param_get(void *p, int i) { va *e = p; return va_okp(e, i) ? e->eff[i] : 0; }   /* preset + macro */
static void va_param_set(void *p, int i, int v) {    /* re-bases a macro target: the effective value becomes v */
    va *e = p; if (!va_okp(e, i)) return;
    v = va_clampi(v, e->pr[i].min, e->pr[i].max);
    e->val[i] = va_clampi(v - e->add[i], e->pr[i].min, e->pr[i].max);
    va_recalc(e);
    if (e->eff[i] != v) { e->val[i] = v; e->add[i] = 0; e->eff[i] = v; }   /* at a range edge: drop that macro share */
}
/* "1.0 ms", "80.6 ms", "352 ms", "1.25 s", "6.3 s", "20 s" (trailing zeros of seconds dropped) */
static void va_fmt_time(char *b, float s) {
    if (s < 0.09995f) snprintf(b, 48, "%.1f ms", s * 1000.f);
    else if (s < 0.9995f) snprintf(b, 48, "%d ms", (int)(s * 1000.f + 0.5f));
    else {
        snprintf(b, 48, s < 9.995f ? "%.2f" : "%.1f", s);
        char *d = strchr(b, '.');
        if (d) { char *z = b + strlen(b) - 1; while (z > d && *z == '0') *z-- = 0; if (z == d) *z = 0; }
        strcat(b, " s");
    }
}
static const char *va_param_text(void *p, int i, int v) {
    va *e = p;
    char *b = e->tb[e->ti]; e->ti = (e->ti + 1) & 7;
    b[0] = 0;
    if (!va_okp(e, i)) return b;
    const va_ps *s = &e->sp->ps[i];
    v = va_clampi(v, e->pr[i].min, e->pr[i].max);
    switch (s->fmt) {
    case F_PCT: snprintf(b, 48, "%d %%", (v * 100 + 63) / 127); break;
    case F_BIP: if (v == 0) snprintf(b, 48, "0 %%"); else snprintf(b, 48, "%+d %%", v < 0 ? -((-v * 100 + 32) / 64) : (v * 100 + 31) / 63); break;
    case F_TATK: case F_TDEC: case F_TREL: va_fmt_time(b, va_env_time(s->fmt - F_TATK, (float)v)); break;
    case F_GLIDE: case F_LFODEL: if (v == 0) snprintf(b, 48, "OFF"); else va_fmt_time(b, va_time((float)v)); break;
    case F_LFORATE:
        if (s->aux >= 0 && e->eff[s->aux]) snprintf(b, 48, "%s", VA_SYNC[va_syncidx(v)].name);
        else { float hz = va_lfohz((float)v); snprintf(b, 48, hz < 1.f ? "%.2f Hz" : hz < 10.f ? "%.1f Hz" : "%.0f Hz", hz); }
        break;
    case F_SEMI: if (v == 0) snprintf(b, 48, "0 st"); else snprintf(b, 48, "%+d st", v); break;
    case F_ST: snprintf(b, 48, "%d st", v); break;
    case F_OCT: if (v == 0) snprintf(b, 48, "0 oct"); else snprintf(b, 48, "%+d oct", v); break;
    case F_CENT: if (v == 0) snprintf(b, 48, "0 ct"); else snprintf(b, 48, "%+d ct", v); break;
    case F_CHOICE: case F_TOGGLE: snprintf(b, 48, "%s", s->ch ? s->ch[v - s->min] : (v ? "ON" : "OFF")); break;
    case F_LEVEL: if (v == 0) snprintf(b, 48, "-inf dB"); else snprintf(b, 48, "%+.1f dB", 20.f * log10f(va_level_gain((float)v))); break;
    case F_PAN:
        if (v == 0) snprintf(b, 48, "C");
        else snprintf(b, 48, "%c %d", v < 0 ? 'L' : 'R', v < 0 ? (-v * 100 + 32) / 64 : (v * 100 + 31) / 63);
        break;
    case F_CUTOFF: { float hz = va_cut_hz((float)v);
        snprintf(b, 48, hz < 1000.f ? "%.0f Hz" : hz < 10000.f ? "%.2f kHz" : "%.1f kHz", hz < 1000.f ? hz : hz * 0.001f); break; }
    case F_AMT: if (v == 0) snprintf(b, 48, "0"); else snprintf(b, 48, "%+d", v); break;
    case F_INT: snprintf(b, 48, "%d", v); break;
    case F_VOICES: snprintf(b, 48, "%d VOICES", v); break;
    case F_SRC: snprintf(b, 48, "%s", VA_SRCNAME[(int)e->sp->src[v]]); break;
    case F_DST: snprintf(b, 48, "%s", e->sp->dst[v]); break;
    case F_PHASE: snprintf(b, 48, "%d deg", (v * 360 + 64) / 128); break;
    case F_MORPH:                        /* SEM mode: LP (0) .. NOTCH (64) .. HP (127) */
        if (v <= 1) snprintf(b, 48, "LP");
        else if (v >= 62 && v <= 66) snprintf(b, 48, "NOTCH");
        else if (v >= 126) snprintf(b, 48, "HP");
        else if (v < 64) snprintf(b, 48, "LP-NOTCH %d %%", (v * 100 + 32) / 64);
        else snprintf(b, 48, "NOTCH-HP %d %%", ((v - 64) * 100 + 31) / 63);
        break;
    case F_HZ: { float hz = va_lfohz((float)v); snprintf(b, 48, hz < 1.f ? "%.2f Hz" : hz < 10.f ? "%.1f Hz" : "%.0f Hz", hz); break; }
    case F_PW: snprintf(b, 48, "%d %%", (int)(5.f + 90.f * v / 127.f + 0.5f)); break;
    }
    return b;
}

/* ------------------------------------------------------------------ macros, presets */
static int va_nmacros(void *p) { (void)p; return 8; }
static const char *va_macro_name(void *p, int m) { va *e = p; return (m >= 0 && m < 8) ? e->sp->macros[m].name : ""; }
static void va_macro_set(void *p, int m, int v) {
    va *e = p; if (m < 0 || m >= 8) return;
    e->mac[m] = va_clampi(v, -64, 63); va_recalc(e);
}
static int va_npresets(void *p) { return ((va *)p)->sp->npre; }
static const char *va_preset_name(void *p, int i) { va *e = p; return (i >= 0 && i < e->sp->npre) ? e->sp->pre[i].name : ""; }
static const char *va_preset_cat(void *p, int i) { va *e = p; return (i >= 0 && i < e->sp->npre) ? e->sp->pre[i].cat : ""; }
static int va_preset_flags(void *p, int i) { va *e = p; return (i >= 0 && i < e->sp->npre) ? e->sp->pre[i].flags : 0; }
static const char *va_preset_arp(void *p, int i) {
    va *e = p; return (i >= 0 && i < e->sp->npre && (e->sp->pre[i].flags & JF_ARP)) ? e->sp->pre[i].arp : NULL;
}
/* sets every parameter (defaults, then the preset's values). Macro offsets return to 0: the core zeroes its macro
   knobs on a preset load (jam.c load_preset), so the engine does the same to stay in step. */
static void va_preset_load(void *p, int i) {
    va *e = p; const va_spec *sp = e->sp;
    if (i < 0 || i >= sp->npre) return;
    for (int k = 0; k < sp->np; k++) e->val[k] = sp->ps[k].def;
    for (const short *kv = sp->pre[i].kv; kv[0] >= 0; kv += 2) if (kv[0] < sp->np) e->val[kv[0]] = kv[1];
    for (int k = 0; k < sp->np; k++) e->val[k] = va_clampi(e->val[k], e->pr[k].min, e->pr[k].max);
    memset(e->mac, 0, sizeof e->mac);
    va_recalc(e);
    e->snap = 1;
}

/* ------------------------------------------------------------------ envelopes, LFOs */
static void va_env_adv(va_env *en, float A, float D, float S, float R, float dts) {
    switch (en->st) {
    case 1: { float c = expf(-1.7918f * dts / A); en->v = 1.2f - (1.2f - en->v) * c; if (en->v >= 1.f) { en->v = 1.f; en->st = 2; } break; }
    case 2: {
        float c = expf(-6.9078f * dts / D);
        if (en->v > S) { float t = S - 0.001f; en->v = t + (en->v - t) * c; if (en->v < S) en->v = S; }
        else { float t = S + 0.001f; en->v = t + (en->v - t) * c; if (en->v > S) en->v = S; }
        break;
    }
    case 3: { float c = expf(-6.9078f * dts / R); en->v = -0.001f + (en->v + 0.001f) * c; if (en->v <= 0.f) { en->v = 0.f; en->st = 0; } break; }
    default: en->v = 0.f; break;
    }
}
static inline float va_lfo_wave(int w, float ph, float sh, float sh0) {
    switch (w) {
    case 0: return va_sin(ph);
    case 1: return ph < 0.25f ? 4.f * ph : ph < 0.75f ? 2.f - 4.f * ph : 4.f * ph - 4.f;
    case 2: return 2.f * ph - 1.f;
    case 3: return 1.f - 2.f * ph;
    case 4: return ph < 0.5f ? 1.f : -1.f;
    case 5: return sh;
    default: { float s = ph * ph * (3.f - 2.f * ph); return sh0 + (sh - sh0) * s; }
    }
}
static float va_lfo_rate(va *e, int k, float modoct) {
    int b = e->sp->p_lfo[k];
    if (e->eff[b + 2]) return (float)(e->bpm / 60.0) / VA_SYNC[va_syncidx(e->eff[b + 1])].beats * va_exp2(modoct);
    return va_lfohz(e->ps[b + 1]) * va_exp2(modoct);
}

/* ------------------------------------------------------------------ lifecycle */
static void va_reset(void *p) {
    va *e = p;
    for (int i = 0; i < VA_NV; i++) {
        va_voice *v = &e->v[i];
        float vr[6]; memcpy(vr, v->vr, sizeof vr);
        memset(v, 0, sizeof *v);
        memcpy(v->vr, vr, sizeof vr); v->slot = i; v->state = V_IDLE;
        v->rng = (0x9E3779B9u * (uint32_t)(i + 1)) ^ 0x5bd1e995u;   /* reset = a deterministic restart */
        if (!v->rng) v->rng = 1u;
    }
    e->nev = 0; e->evseq = 0;
    e->wheel = e->at = e->bend = e->wheel_s = e->at_s = e->bend_s = e->bend_st = 0.f;
    e->sustain = 0; e->nheld = 0; e->nmono = 0; e->mono_note = -1; e->have_last = 0;
    e->clk = 0;
    for (int k = 0; k < 3; k++) { e->gph[k] = 0.f; e->gsh[k] = e->gsh0[k] = 0.f; e->glfo[k] = 0.f; }
    e->grng = 0x2545F491u;
    e->lim = 0.f; e->dcx[0] = e->dcx[1] = e->dcy[0] = e->dcy[1] = 0.f;
    e->snap = 1;
    e->lev = va_level_gain((float)e->eff[e->sp->p_level]); e->levd = 0.f;
    if (e->sp->greset) e->sp->greset(e);
}
static void *va_create(const va_spec *sp, double sr) {
    if (sr < 8000.0 || sr > 384000.0) return NULL;
    va *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->sp = sp; e->sr = (float)sr; e->isr = 1.f / e->sr;
    for (int i = 0; i < sp->np; i++) {
        const va_ps *s = &sp->ps[i];
        e->pr[i].min = s->min; e->pr[i].max = s->max;
        e->pr[i].kind = (s->fmt == F_CHOICE || s->fmt == F_SRC || s->fmt == F_DST) ? JP_SWITCH : s->fmt == F_TOGGLE ? JP_TOGGLE : JP_KNOB;
        if (s->fmt == F_SRC) { e->pr[i].min = 0; e->pr[i].max = sp->nsrc - 1; }
        if (s->fmt == F_DST) { e->pr[i].min = 0; e->pr[i].max = sp->ndst - 1; }
    }
    float tick = VA_CR / e->sr;
    e->smc = 1.f - expf(-tick / 0.012f);      /* parameter smoothing: 12 ms */
    e->ccc = 1.f - expf(-tick / 0.008f);      /* mod wheel / aftertouch: 8 ms */
    e->bdc = 1.f - expf(-tick / 0.004f);      /* pitch bend: 4 ms */
    e->limA = 1.f - expf(-1.f / (0.0007f * e->sr));
    e->limR = 1.f - expf(-1.f / (0.15f * e->sr));
    e->dcR = 1.f - 2.f * VA_PI * 6.f / e->sr;
    e->bpm = 120.0;
    uint32_t seed = 0x9E3779B9u;
    for (int i = 0; i < VA_NV; i++) {
        e->v[i].rng = va_rng(&seed) | 1u;
        for (int k = 0; k < 6; k++) e->v[i].vr[k] = va_noise(&seed);
    }
    va_preset_load(e, 0);
    for (int i = 0; i < sp->np; i++) e->ps[i] = (float)e->eff[i];
    if (sp->init) sp->init(e);
    va_reset(e);
    return e;
}
static void va_destroy(void *p) { free(p); }

/* ------------------------------------------------------------------ control-rate tick */
static void va_vtick(va *e, va_voice *v, int n) {
    const va_spec *sp = e->sp;
    float dts = (float)n * e->isr;
    /* glide */
    if (v->pitch != v->ptgt) {
        int gv = sp->p_glide >= 0 ? e->eff[sp->p_glide] : 0;
        if (gv <= 0) v->pitch = v->ptgt;
        else {
            float c = expf(-dts * 3.f / va_time((float)gv));
            v->pitch = v->ptgt + (v->pitch - v->ptgt) * c;
            if (fabsf(v->pitch - v->ptgt) < 1e-4f) v->pitch = v->ptgt;
        }
    }
    /* analog drift: slow random walk per voice */
    float slop = sp->p_slop >= 0 ? e->ps[sp->p_slop] * (1.f / 127.f) : 0.f;
    v->driftt -= dts;
    if (v->driftt <= 0.f) { v->driftt = 0.15f + 0.35f * (va_noise(&v->rng) * 0.5f + 0.5f); v->dtgt = va_noise(&v->rng); }
    v->drift += (v->dtgt - v->drift) * (1.f - expf(-dts / 0.4f));
    /* stack / voice detune */
    float det = 0.f;
    if (sp->p_detune >= 0) {
        float dc = e->ps[sp->p_detune] * (1.f / 127.f);
        dc = dc * dc * 0.5f;                                   /* up to +-50 ct at the outer stack voices */
        det = v->stackpos * dc;
        if (sp->poly_det > 0.f && v->stackpos == 0.f) det = dc * sp->poly_det * v->vr[5];
    }
    /* a macro that pushes SLOP up (ANALOG) also widens the slow drift (to ~ +-25 c at +63), so it is heard on one held note;
       macro 0 = the preset's own drift */
    float sx = sp->p_slop >= 0 ? va_clampf((float)e->add[sp->p_slop] * (1.f / 127.f), 0.f, 1.f) : 0.f;
    v->det = det + slop * (0.10f * v->vr[0] + 0.05f * v->drift) + sx * 2.5f * v->drift;
    v->pbase = v->pitch + e->bend_st + v->det;
    /* envelopes (times scaled by the matrix and the analog variation) */
    float tsa = 1.f, tsd = 1.f, tsr = 1.f, vs = 1.f + slop * 0.12f * v->vr[2];
    if (sp->d_atk > 0) tsa = va_exp2(4.f * v->mod[sp->d_atk]);
    if (sp->d_dec > 0) tsd = va_exp2(4.f * v->mod[sp->d_dec]);
    if (sp->d_rel > 0) tsr = va_exp2(4.f * v->mod[sp->d_rel]);
    for (int k = 0; k < sp->nenv; k++) {
        int b = sp->p_env[k];
        va_env_adv(&v->env[k], va_env_time(0, e->eff[b]) * tsa * vs, va_env_time(1, e->eff[b + 1]) * tsd * vs,
                   e->ps[b + 2] * (1.f / 127.f), va_env_time(2, e->eff[b + 3]) * tsr * vs, dts);
    }
    /* LFOs */
    for (int k = 0; k < sp->nlfo; k++) {
        int b = sp->p_lfo[k];
        float y;
        if (e->eff[b + 4]) {                                   /* KEY: per-voice phase */
            float rm = sp->d_lforate[k] > 0 ? 4.f * v->mod[sp->d_lforate[k]] : 0.f;
            v->lph[k] += va_lfo_rate(e, k, rm) * dts;
            if (v->lph[k] >= 1.f) { v->lph[k] -= floorf(v->lph[k]); v->lsh0[k] = v->lsh[k]; v->lsh[k] = va_noise(&v->rng); }
            y = va_lfo_wave(e->eff[b], v->lph[k], v->lsh[k], v->lsh0[k]);
        } else y = e->glfo[k];
        int dl = e->eff[b + 3];
        if (dl > 0) { float f = v->t / va_time((float)dl); if (f < 1.f) y *= f * f; }
        v->lfo[k] = y;
    }
    v->t += dts;
    /* sources */
    float *s = v->src;
    s[S_OFF] = 0.f; s[S_LFO1] = v->lfo[0]; s[S_LFO2] = v->lfo[1]; s[S_LFO3] = v->lfo[2];
    s[S_AENV] = v->env[0].v; s[S_FENV] = v->env[1].v; s[S_MENV] = sp->nenv > 2 ? v->env[2].v : v->env[1].v;
    s[S_VEL] = v->vel; s[S_NOTE] = (v->pitch - 60.f) * (1.f / 60.f); s[S_WHEEL] = e->wheel_s; s[S_AT] = e->at_s;
    s[S_RAND] = v->rnd; s[S_BEND] = e->bend_s; s[S_STACK] = v->stackpos;
    /* matrix: pass 1 = LFO depth destinations (a routed LFO AMT acts as that LFO's VCA), pass 2 = the rest */
    float *m = v->mod;
    memset(m, 0, sizeof(float) * sp->ndst);
    const int *ev = e->eff + sp->p_mod;
    int depth_routed[3] = {0, 0, 0};
    for (int k = 0; k < 8; k++) {
        int si = ev[k * 3], di = ev[k * 3 + 1], am = ev[k * 3 + 2];
        if (!si || !di || !am) continue;
        for (int l = 0; l < sp->nlfo; l++) if (di == sp->d_lfoamt[l] && sp->src[si] != S_LFO1 + l) {
            float a = am * 0.01f; if ((sp->dquad >> di) & 1ull) a *= fabsf(a);
            m[di] += a * s[(int)sp->src[si]]; depth_routed[l] = 1;
        }
    }
    for (int l = 0; l < sp->nlfo; l++) if (depth_routed[l]) s[S_LFO1 + l] *= va_clampf(m[sp->d_lfoamt[l]], -1.f, 1.f);
    for (int k = 0; k < 8; k++) {
        int si = ev[k * 3], di = ev[k * 3 + 1], am = ev[k * 3 + 2];
        if (!si || !di || !am) continue;
        int isd = 0;
        for (int l = 0; l < sp->nlfo; l++) if (di == sp->d_lfoamt[l]) isd = 1;
        if (isd) continue;
        float a = am * 0.01f; if ((sp->dquad >> di) & 1ull) a *= fabsf(a);
        m[di] += a * s[(int)sp->src[si]];
    }
    sp->vtick(e, v, n);
    if (v->env[0].st == 0 || (v->env[0].st == 2 && v->env[0].v <= 0.f)) v->dying = 1;
}
static void va_gtick(va *e) {
    const va_spec *sp = e->sp;
    if (e->snap) { for (int i = 0; i < sp->np; i++) e->ps[i] = (float)e->eff[i]; e->snap = 0; }
    else {
        float c = e->smc;
        for (int i = 0; i < sp->np; i++) {
            float t = (float)e->eff[i], d = t - e->ps[i];
            e->ps[i] = fabsf(d) < 1e-3f ? t : e->ps[i] + d * c;
        }
    }
    e->wheel_s += (e->wheel - e->wheel_s) * e->ccc;
    e->at_s += (e->at - e->at_s) * e->ccc;
    e->bend_s += (e->bend - e->bend_s) * e->bdc;
    if (fabsf(e->bend_s - e->bend) < 1e-5f) e->bend_s = e->bend;
    e->bend_st = e->bend_s * (float)(sp->p_bend >= 0 ? e->eff[sp->p_bend] : 2);
    float dts = VA_CR * e->isr;
    for (int k = 0; k < sp->nlfo; k++) {
        int b = sp->p_lfo[k];
        e->gph[k] += va_lfo_rate(e, k, 0.f) * dts;
        if (e->gph[k] >= 1.f) { e->gph[k] -= floorf(e->gph[k]); e->gsh0[k] = e->gsh[k]; e->gsh[k] = va_noise(&e->grng); }
        float ph = va_wrap1(e->gph[k] + e->eff[b + 5] * (1.f / 128.f));
        e->glfo[k] = va_lfo_wave(e->eff[b], ph, e->gsh[k], e->gsh0[k]);
    }
    if (sp->gtick) sp->gtick(e);
}

/* ------------------------------------------------------------------ voice allocation */
static int va_mode(va *e) { return e->sp->p_voices >= 0 ? e->eff[e->sp->p_voices] : 0; }
static void va_fadeout(va *e, va_voice *v) {
    if (v->state == V_IDLE || v->state == V_FADE) return;
    v->state = V_FADE; v->fstep = 1.f / (0.003f * e->sr);
}
static va_voice *va_alloc(va *e) {
    int active = 0;
    va_voice *idle = NULL;
    for (int i = 0; i < VA_NV; i++) {
        va_voice *v = &e->v[i];
        if (v->state == V_HELD || v->state == V_REL) active++;
        else if (v->state == V_IDLE && !idle) idle = v;
    }
    if (active >= VA_POLY || !idle) {
        if (active >= VA_POLY) {
            va_voice *vic = NULL;
            for (int i = 0; i < VA_NV; i++) {          /* quietest released voice */
                va_voice *v = &e->v[i];
                if (v->state == V_REL && (!vic || v->env[0].v < vic->env[0].v)) vic = v;
            }
            if (!vic) for (int i = 0; i < VA_NV; i++) {  /* else the oldest held one */
                va_voice *v = &e->v[i];
                if (v->state == V_HELD && (!vic || v->age < vic->age)) vic = v;
            }
            if (vic) va_fadeout(e, vic);
        }
        if (!idle) {                                     /* every slot busy: reuse the most faded slot (rare) */
            va_voice *f = NULL;
            for (int i = 0; i < VA_NV; i++) if (e->v[i].state == V_FADE && (!f || e->v[i].fade < f->fade)) f = &e->v[i];
            if (!f) f = &e->v[0];
            f->state = V_IDLE;
            idle = f;
        }
    }
    return idle;
}
static unsigned va_to_grid(va *e) { unsigned r = e->clk % VA_CR; return r ? VA_CR - r : 0; }
static const float VA_PANPAT[VA_NV] = {-1.f, 1.f, -0.5f, 0.5f, -0.75f, 0.75f, -0.25f, 0.25f, -0.875f, 0.875f,
                                       -0.375f, 0.375f, -0.625f, 0.625f, -0.125f, 0.125f, -1.f, 1.f, -0.5f, 0.5f};
static void va_start(va *e, va_voice *v, int note, float vel, float stackpos, int fresh) {
    const va_spec *sp = e->sp;
    if (fresh) {
        float vr[6]; memcpy(vr, v->vr, sizeof vr); uint32_t r = v->rng; int slot = v->slot;
        memset(v, 0, offsetof(va_voice, x));          /* x (the engine's DSP state) is reset by vstart(fresh) */
        memcpy(v->vr, vr, sizeof vr); v->rng = r; v->slot = slot;
        v->fade = 1.f;
        v->pitch = (e->have_last && sp->p_glide >= 0 && e->eff[sp->p_glide] > 0) ? e->last_pitch : (float)note;
        v->drift = 0.f; v->driftt = 0.f;
        for (int k = 0; k < 3; k++) { v->lsh[k] = va_noise(&v->rng); v->lsh0[k] = v->lsh[k]; }
    }
    v->state = V_HELD; v->sustained = 0; v->dying = 0;
    v->note = note; v->ptgt = (float)note; v->vel = vel; v->stackpos = stackpos;
    if (fresh) v->sgain = e->next_sgain > 0.f ? e->next_sgain : 1.f;
    v->age = ++e->agec;
    v->rnd = va_noise(&v->rng);
    float sp_ = sp->p_spread >= 0 ? e->eff[sp->p_spread] / 127.f : 0.f;
    float pan = sp->p_pan >= 0 ? e->eff[sp->p_pan] / 64.f : 0.f;
    v->pan = va_clampf(pan + sp_ * (stackpos != 0.f ? stackpos : VA_PANPAT[v->slot]), -1.f, 1.f);
    for (int k = 0; k < 3; k++) v->env[k].st = 1;
    v->t = 0.f;
    for (int k = 0; k < sp->nlfo; k++) if (e->eff[sp->p_lfo[k] + 4]) {
        v->lph[k] = e->eff[sp->p_lfo[k] + 5] * (1.f / 128.f);
    }
    e->last_pitch = (float)note; e->have_last = 1;
    sp->vstart(e, v, fresh);
    unsigned r = va_to_grid(e);
    if (fresh && r) va_vtick(e, v, (int)r);
}
static void va_retrig(va *e, va_voice *v, int note, float vel) {   /* same voice, envelopes restart from where they are */
    v->note = note; v->ptgt = (float)note; v->vel = vel; v->sustained = 0; v->dying = 0; v->state = V_HELD;
    for (int k = 0; k < 3; k++) v->env[k].st = 1;
    v->t = 0.f;
    for (int k = 0; k < e->sp->nlfo; k++) if (e->eff[e->sp->p_lfo[k] + 4]) v->lph[k] = e->eff[e->sp->p_lfo[k] + 5] * (1.f / 128.f);
    v->age = ++e->agec;
    e->last_pitch = (float)note; e->have_last = 1;
    e->sp->vstart(e, v, 0);
}
static void va_release(va *e, va_voice *v) {
    (void)e;
    if (v->state != V_HELD) return;
    v->state = V_REL; v->sustained = 0;
    for (int k = 0; k < 3; k++) if (v->env[k].st) v->env[k].st = 3;
}
static int va_stack_n(va *e) {
    int m = va_mode(e);
    if (m == 2) return e->sp->p_stack >= 0 ? va_clampi(e->eff[e->sp->p_stack], 1, 8) : 4;
    return 1;
}
static void va_mono_group_start(va *e, int note, float vel) {
    int k = va_stack_n(e);
    e->nmono = 0;
    for (int i = 0; i < k; i++) {
        va_voice *v = va_alloc(e);
        float pos = k > 1 ? -1.f + 2.f * (float)i / (float)(k - 1) : 0.f;
        e->next_sgain = 1.f / sqrtf((float)k);
        va_start(e, v, note, vel, pos, 1);
        e->next_sgain = 1.f;
        e->mono[e->nmono++] = (int)(v - e->v);
    }
    e->mono_note = note;
}
static int va_mono_alive(va *e) {     /* group exists and its gate is open */
    if (e->nmono <= 0) return 0;
    va_voice *v = &e->v[e->mono[0]];
    return v->state == V_HELD;
}
static void va_mono_to(va *e, int note, float vel, int retrig) {
    int leg = e->sp->p_legato >= 0 ? e->eff[e->sp->p_legato] : 0;
    for (int i = 0; i < e->nmono; i++) {
        va_voice *v = &e->v[e->mono[i]];
        if (v->state != V_HELD) continue;
        if (retrig && !leg) va_retrig(e, v, note, vel);
        else { v->note = note; v->ptgt = (float)note; v->sustained = 0; }
        if (!(e->sp->p_glide >= 0 && e->eff[e->sp->p_glide] > 0)) v->pitch = (float)note;
    }
    e->mono_note = note; e->last_pitch = (float)note; e->have_last = 1;
}
static void va_note_on(va *e, int note, int vel) {
    float vf = vel / 127.f;
    if (va_mode(e) == 0) {
        for (int i = 0; i < VA_NV; i++) {
            va_voice *v = &e->v[i];
            if (v->state == V_HELD && v->note == note) { va_retrig(e, v, note, vf); return; }
        }
        va_voice *v = va_alloc(e);
        va_start(e, v, note, vf, 0.f, 1);
        return;
    }
    /* mono / unison: key stack, last-note priority */
    int j = 0;
    for (int i = 0; i < e->nheld; i++) if (e->held[i] != note) { e->held[j] = e->held[i]; e->heldv[j] = e->heldv[i]; j++; }
    e->nheld = j;
    if (e->nheld < 32) { e->held[e->nheld] = note; e->heldv[e->nheld] = vel; e->nheld++; }
    if (va_mono_alive(e)) {
        int any_key = e->nheld > 1;                       /* another key still down: legato applies */
        int leg = e->sp->p_legato >= 0 ? e->eff[e->sp->p_legato] : 0;
        if (any_key && leg) va_mono_to(e, note, vf, 0);
        else for (int i = 0; i < e->nmono; i++) va_retrig(e, &e->v[e->mono[i]], note, vf), e->mono_note = note;
        if (!(e->sp->p_glide >= 0 && e->eff[e->sp->p_glide] > 0)) for (int i = 0; i < e->nmono; i++) e->v[e->mono[i]].pitch = (float)note;
    } else va_mono_group_start(e, note, vf);
}
static int va_in_mono(va *e, int idx) { for (int i = 0; i < e->nmono; i++) if (e->mono[i] == idx) return 1; return 0; }
static void va_note_off(va *e, int note) {
    int mono = va_mode(e) != 0;
    for (int i = 0; i < VA_NV; i++) {          /* poly voices (and any left over from a mode change) */
        va_voice *v = &e->v[i];
        if (mono && va_in_mono(e, i)) continue;
        if (v->state == V_HELD && v->note == note && !v->sustained) {
            if (e->sustain) v->sustained = 1; else va_release(e, v);
        }
    }
    if (!mono) return;
    int j = 0, was = 0;
    for (int i = 0; i < e->nheld; i++) { if (e->held[i] != note) { e->held[j] = e->held[i]; e->heldv[j] = e->heldv[i]; j++; } else was = 1; }
    e->nheld = j;
    if (!was || note != e->mono_note) return;
    if (e->nheld > 0) { va_mono_to(e, e->held[e->nheld - 1], e->heldv[e->nheld - 1] / 127.f, 1); return; }
    for (int i = 0; i < e->nmono; i++) {
        va_voice *v = &e->v[e->mono[i]];
        if (v->state != V_HELD) continue;
        if (e->sustain) v->sustained = 1; else va_release(e, v);
    }
}
static void va_all_off(va *e, int hard) {
    e->nheld = 0; e->sustain = 0;
    for (int i = 0; i < VA_NV; i++) { if (hard) va_fadeout(e, &e->v[i]); else va_release(e, &e->v[i]); }
}
static void va_event(va *e, const va_ev *x) {
    const va_spec *sp = e->sp;
    switch (x->st) {
    case 0x90: if (x->d2 > 0) { va_note_on(e, x->d1, x->d2); break; } /* fall through: vel 0 = off */
    /* fall through */
    case 0x80: va_note_off(e, x->d1); break;
    case 0xB0:
        if (x->d1 == 1) e->wheel = x->d2 / 127.f;
        else if (x->d1 == 64) {
            int on = x->d2 >= 64;
            if (e->sustain && !on) {
                e->sustain = 0;
                for (int i = 0; i < VA_NV; i++) if (e->v[i].state == V_HELD && e->v[i].sustained) va_release(e, &e->v[i]);
                if (va_mode(e) && e->nheld == 0) for (int i = 0; i < e->nmono; i++) va_release(e, &e->v[e->mono[i]]);
            }
            e->sustain = on;
        } else if (x->d1 == 74 && sp->p_cutoff >= 0) {
            int i = sp->p_cutoff;
            va_param_set(e, i, e->pr[i].min + x->d2 * (e->pr[i].max - e->pr[i].min) / 127);
        } else if (x->d1 == 123) va_all_off(e, 0);
        else if (x->d1 == 120) va_all_off(e, 1);
        break;
    case 0xD0: e->at = x->d1 / 127.f; break;
    case 0xE0: e->bend = (float)(((x->d2 & 127) << 7 | (x->d1 & 127)) - 8192) / 8192.f; if (e->bend < -1.f) e->bend = -1.f; break;
    }
}
static void va_midi(void *p, int frame, int st, int d1, int d2) {
    va *e = p;
    int s = st & 0xF0;
    if (s != 0x80 && s != 0x90 && s != 0xB0 && s != 0xD0 && s != 0xE0) return;
    if ((s == 0x80 || s == 0x90) && (d1 < 0 || d1 > 127)) return;
    if (e->nev >= VA_MAXEV) return;
    if (frame < 0) frame = 0;
    if (frame > VA_BLOCK - 1) frame = VA_BLOCK - 1;
    va_ev *x = &e->ev[e->nev++];
    x->frame = frame; x->st = s; x->d1 = d1 & 127; x->d2 = d2 < 0 ? 0 : d2 > 127 ? 127 : d2; x->seq = e->evseq++;
}

/* ------------------------------------------------------------------ render */
static void va_chunk(va *e, float *L, float *R, int len) {
    const va_spec *sp = e->sp;
    memset(L, 0, sizeof(float) * len); memset(R, 0, sizeof(float) * len);
    for (int i = 0; i < VA_NV; i++) {
        va_voice *v = &e->v[i];
        if (v->state == V_IDLE) continue;
        sp->vrender(e, v, e->tL, e->tR, len);
        if (v->state == V_FADE) {
            float f = v->fade, st = v->fstep;
            for (int k = 0; k < len; k++) {
                L[k] += e->tL[k] * f; R[k] += e->tR[k] * f;
                f -= st; if (f < 0.f) f = 0.f;
            }
            v->fade = f;
            if (f <= 0.f) v->state = V_IDLE;
        } else {
            for (int k = 0; k < len; k++) { L[k] += e->tL[k]; R[k] += e->tR[k]; }
        }
    }
    float lv = e->lev, ld = e->levd;
    for (int k = 0; k < len; k++) { L[k] *= lv; R[k] *= lv; lv += ld; }
    e->lev = lv;
    if (sp->fx) sp->fx(e, L, R, len);
}
static void va_span(va *e, float *L, float *R, int len) {
    while (len > 0) {
        unsigned ph = e->clk % VA_CR;
        if (ph == 0) {
            va_gtick(e);
            for (int i = 0; i < VA_NV; i++) {
                va_voice *v = &e->v[i];
                if (v->state == V_IDLE) continue;
                if (v->dying && v->state != V_FADE) { v->state = V_IDLE; continue; }
                va_vtick(e, v, VA_CR);
            }
            float lt = va_level_gain(e->ps[e->sp->p_level]);
            e->levd = (lt - e->lev) * (1.f / VA_CR);
        }
        int n = (int)(VA_CR - ph); if (n > len) n = len;
        va_chunk(e, L, R, n);
        L += n; R += n; len -= n; e->clk += (unsigned)n;
        if (e->clk % VA_CR == 0) {               /* end of a tick: voices that finished their release go idle */
            for (int i = 0; i < VA_NV; i++) {
                va_voice *v = &e->v[i];
                if (v->state != V_IDLE && v->state != V_FADE && v->dying) v->state = V_IDLE;
            }
            e->levd = 0.f; e->lev = va_level_gain(e->ps[e->sp->p_level]);
        }
    }
}
static void va_render(void *p, float *L, float *R, int n) {
    va *e = p;
    if (n > VA_BLOCK) n = VA_BLOCK;
    if (n <= 0) return;
    /* events in frame order (insertion sort, stable by arrival) */
    for (int i = 1; i < e->nev; i++) {
        va_ev x = e->ev[i]; int j = i - 1;
        while (j >= 0 && (e->ev[j].frame > x.frame)) { e->ev[j + 1] = e->ev[j]; j--; }
        e->ev[j + 1] = x;
    }
    int pos = 0;
    for (int i = 0; i < e->nev; i++) {
        int f = e->ev[i].frame; if (f >= n) f = n - 1;
        if (f > pos) { va_span(e, L + pos, R + pos, f - pos); pos = f; }
        va_event(e, &e->ev[i]);
    }
    e->nev = 0;
    if (pos < n) va_span(e, L + pos, R + pos, n - pos);
    /* output: glue limiter (threshold 0.7, 0.7 ms attack, 150 ms release), DC block (6 Hz), soft clip
       (linear to 0.6, ceiling 0.89 = -1 dBFS) */
    float lim = e->lim, A = e->limA, Rl = e->limR, R0 = e->dcR, pk = 0.f;
    float x1l = e->dcx[0], y1l = e->dcy[0], x1r = e->dcx[1], y1r = e->dcy[1];
    for (int i = 0; i < n; i++) {
        float l = L[i], r = R[i];
        float yl = l - x1l + R0 * y1l; x1l = l; y1l = yl;
        float yr = r - x1r + R0 * y1r; x1r = r; y1r = yr;
        float a = fabsf(yl) > fabsf(yr) ? fabsf(yl) : fabsf(yr);
        lim += (a - lim) * (a > lim ? A : Rl);
        float g = lim > 0.7f ? 0.7f / lim + (1.f - 0.7f / lim) * 0.25f : 1.f;   /* 4:1 above the threshold */
        yl *= g; yr *= g;
        float ay = fabsf(yl);
        if (ay > 0.6f) { float o = 0.6f + 0.29f * va_tanh((ay - 0.6f) / 0.29f); yl = yl < 0.f ? -o : o; }
        ay = fabsf(yr);
        if (ay > 0.6f) { float o = 0.6f + 0.29f * va_tanh((ay - 0.6f) / 0.29f); yr = yr < 0.f ? -o : o; }
        L[i] = yl; R[i] = yr;
        a = fabsf(yl) > fabsf(yr) ? fabsf(yl) : fabsf(yr);
        if (a > pk) pk = a;
    }
    if (fabsf(x1l) < 1e-20f) x1l = 0.f;
    if (fabsf(y1l) < 1e-20f) y1l = 0.f;
    if (fabsf(x1r) < 1e-20f) x1r = 0.f;
    if (fabsf(y1r) < 1e-20f) y1r = 0.f;
    if (lim < 1e-20f) lim = 0.f;
    e->dcx[0] = x1l; e->dcy[0] = y1l; e->dcx[1] = x1r; e->dcy[1] = y1r; e->lim = lim;
    e->pk = pk;
}
static void va_tempo(void *p, double bpm, int playing, double beat) {
    va *e = p;
    if (bpm > 1.0 && bpm < 1000.0) e->bpm = bpm;
    int start = playing && !e->playing;
    e->playing = playing;
    if (!playing) return;
    for (int k = 0; k < e->sp->nlfo; k++) {          /* synced free LFOs lock their phase to the beat */
        int b = e->sp->p_lfo[k];
        if (!e->eff[b + 2] || e->eff[b + 4]) continue;
        double beats = VA_SYNC[va_syncidx(e->eff[b + 1])].beats;
        double ph = beat / beats; ph -= floor(ph);
        float d = (float)ph - e->gph[k];
        if (start || fabsf(d) > 0.02f) e->gph[k] = (float)ph;   /* re-lock when it has drifted (or at play) */
    }
}
static double va_peak(void *p) { return ((va *)p)->pk; }
/* test helper: counts of voices by state (held, released, fading) */
static int va_dbg(void *p, int *held, int *rel, int *fade) {
    va *e = p; int h = 0, r = 0, f = 0;
    for (int i = 0; i < VA_NV; i++) { int s = e->v[i].state; h += s == V_HELD; r += s == V_REL; f += s == V_FADE; }
    if (held) *held = h;
    if (rel) *rel = r;
    if (fade) *fade = f;
    return h + r;
}

/* live envelope / LFO state for the Sound page (jeng viz; audio thread, after render; v arrives zeroed).
   Voice: the newest held one (highest age), else the newest releasing one, else the newest idle one (reported IDLE).
   Envelopes: va_env_adv is RC style in every segment (attack aims at 1.2 and stops at 1.0, decay/release aim 0.1 %
   past their target), so all three laws are JV_EXP. st 2 is DECAY until the level sits within 1 % of SUSTAIN.
   LFOs: value = v->lfo[k], exactly what the matrix and the engine read (shape, KEY/FREE, DELAY fade-in included,
   before any depth); phase = the phase that value was computed from (global phase + PHASE offset for FREE, the
   voice's own phase for KEY); hz = the rate the engine advances it with (sync, tempo, rate modulation). */
static void va_viz(void *p, jviz_t *o) {
    va *e = p; const va_spec *sp = e->sp;
    va_voice *vh = NULL, *vr = NULL, *vl = NULL;
    for (int i = 0; i < VA_NV; i++) {
        va_voice *v = &e->v[i];
        if (v->state == V_HELD) { if (!vh || v->age > vh->age) vh = v; }
        else if (v->state == V_REL) { if (!vr || v->age > vr->age) vr = v; }
        if (!vl || v->age > vl->age) vl = v;
    }
    va_voice *v = vh ? vh : vr ? vr : vl;
    int live = vh || vr;
    int ne = sp->nenv < JV_MAX ? sp->nenv : JV_MAX;
    o->nenv = ne;
    for (int k = 0; k < ne; k++) {
        int b = sp->p_env[k];
        const va_env *en = &v->env[k];
        float S = e->ps[b + 2] * (1.f / 127.f);
        int st = JV_IDLE;
        if (live && !v->dying) {
            if (en->st == 1) st = JV_ATTACK;
            else if (en->st == 2) st = fabsf(en->v - S) <= 0.01f ? JV_SUSTAIN : JV_DECAY;
            else if (en->st == 3) st = JV_RELEASE;
        }
        o->env[k].page = sp->ps[b].page;
        o->env[k].stage = st;
        o->env[k].level = st == JV_IDLE ? 0.f : va_clampf(en->v, 0.f, 1.f);
        o->env[k].sus = va_clampf(S, 0.f, 1.f);
        o->env[k].alaw = JV_EXP; o->env[k].dlaw = JV_EXP; o->env[k].rlaw = JV_EXP;
    }
    int nl = sp->nlfo < JV_MAX ? sp->nlfo : JV_MAX;
    o->nlfo = nl;
    for (int k = 0; k < nl; k++) {
        int b = sp->p_lfo[k];
        o->lfo[k].page = sp->ps[b].page;
        if (e->eff[b + 4]) {                                   /* KEY: the voice's own phase and rate */
            float rm = sp->d_lforate[k] > 0 ? 4.f * v->mod[sp->d_lforate[k]] : 0.f;
            o->lfo[k].phase = va_wrap1(v->lph[k]);
            o->lfo[k].hz = va_lfo_rate(e, k, rm);
            o->lfo[k].value = live ? v->lfo[k] : va_lfo_wave(e->eff[b], o->lfo[k].phase, v->lsh[k], v->lsh0[k]);
        } else {                                               /* FREE: the global LFO */
            o->lfo[k].phase = va_wrap1(e->gph[k] + e->eff[b + 5] * (1.f / 128.f));
            o->lfo[k].hz = va_lfo_rate(e, k, 0.f);
            o->lfo[k].value = live ? v->lfo[k] : e->glfo[k];
        }
        o->lfo[k].value = va_clampf(o->lfo[k].value, -1.f, 1.f);
    }
}

#define VA_JENG_FIELDS(createfn, npoly)                                                                         \
    .kind = JE_SYNTH, .poly = npoly, .create = createfn, .destroy = va_destroy, .reset = va_reset, .midi = va_midi, \
    .render = va_render, .tempo = va_tempo, .npages = va_npages, .page_name = va_page_name, .nparams = va_nparams,   \
    .param_page = va_param_page, .param_name = va_param_name, .param_kind = va_param_kind, .param_min = va_param_min, \
    .param_max = va_param_max, .param_get = va_param_get, .param_set = va_param_set, .param_text = va_param_text,     \
    .param_basic = va_param_basic, .nmacros = va_nmacros, .macro_name = va_macro_name, .macro_set = va_macro_set,    \
    .npresets = va_npresets, .preset_name = va_preset_name, .preset_cat = va_preset_cat,                             \
    .preset_flags = va_preset_flags, .preset_load = va_preset_load, .preset_arp = va_preset_arp,                     \
    .nlanes = NULL, .lane_name = NULL, .lane_note = NULL, .peak = va_peak, .viz = va_viz

/* ------------------------------------------------------------------ helpers for the engine files */
/* linear ramp over the rest of the tick: cur moves to target in n samples */
static inline void va_ramp(float *cur, float *inc, float target, int n) { *inc = (target - *cur) / (float)n; }
/* the first tick of a fresh voice starts on its targets (pitch, cutoff, PW ...) instead of sweeping from 0 */
static inline void va_ramp0(float *cur, float *inc, float target, int n, int first) {
    if (first) { *cur = target; *inc = 0.f; } else *inc = (target - *cur) / (float)n;
}
static inline float va_tune_dt(va *e, float note) {
    float dt = va_mtof(note) * e->isr;
    return dt > 0.45f ? 0.45f : dt < 1e-7f ? 1e-7f : dt;
}
static inline float va_cut_g(va *e, float semis_above_20hz) {
    float hz = 20.f * va_exp2(semis_above_20hz * (1.f / 12.f));
    float lim = 0.42f * e->sr; if (lim > 20000.f) lim = 20000.f + (lim - 20000.f) * 0.5f;
    if (hz > lim) hz = lim;
    if (hz < 8.f) hz = 8.f;
    return tanf(VA_PI * hz * e->isr);
}
static inline float va_flush(float x) { return fabsf(x) < 1e-15f ? 0.f : x; }
#endif
