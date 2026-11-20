// SPDX-License-Identifier: GPL-3.0-only
// Jammer classic1: shared framework for the classic analog engines AURA, TITAN and SOLO (aura.c, titan.c, solo.c).
// Everything here is static (each engine file gets its own copy); no globals, all state lives in the instance.
//
// Structure
//   params   : engine table (va_ps) + a common block appended at create (LFO 1, LFO 2, VOICE basics, LEVEL, MOD 1..8).
//              base[] = preset/user value, eff[] = clamp(base + macro offsets) (what param_get returns and the DSP uses);
//              a user param_set on a macro target re-bases it (base = v - macro offset), so get == set.
//   smoothing: every knob is smoothed (one pole, 8 ms) on a global 16-sample control grid (sample counter based, so the
//              result does not depend on the host block size).
//   voices   : poly + 4 slots; the 4 extra slots hold stolen voices while they fade out (3 ms), so stealing never clicks.
//              A re-struck note never reuses a voice that is ringing in its release; only a key still held retriggers
//              its own voice (envelopes restart from their current level, analog style). Steal order: the quietest
//              released voice, then the oldest held one. Mono / unison: last-note priority, legato, glide (OFF/ON/AUTO).
//   per voice: control tick every 16 samples (envelopes, LFOs, glide, drift, mod matrix -> engine vctrl targets),
//              per-sample linear interpolation of every control channel (no zipper), engine vdsp per sample.
//   oscillators: band-limited with 2-point polyBLEP / polyBLAMP residuals and one sample of delay (so the residual
//              before a discontinuity can still be added): steps (saw wrap, pulse edges, sync) and corners (triangle).
//   filter   : IR3109-style 4 x OTA one-pole cascade (zero-delay feedback solve, tanh at the OTA input), taps at 12 and
//              24 dB, self-oscillates above k = 4.
//   master   : engine post (AURA: HPF + BBD chorus) -> preset trim * LEVEL (smoothed) -> DC block -> peak limiter
//              (instant attack, 120 ms release, threshold 0.80) -> soft clip (linear to 0.70, ceiling 0.89 = -1 dBFS).
#ifndef JAMMER_VA1_COMMON_H
#define JAMMER_VA1_COMMON_H
struct jviz;                 /* file-scope tag before jeng.h uses it in jeng_def_t.viz's parameter list */
#include "../../core/jeng.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VA_MAXP 150
#define VA_MAXPOLY 16
#define VA_MAXSLOT (VA_MAXPOLY + 4)
#define VA_TICK 16
#define VA_BLOCK 1024
#define VA_MAXEV 512
#define VA_NC 18
#define VA_PI 3.14159265358979f
#define VA_CHBUF 8192
#ifndef VA_STEAL_MS
#define VA_STEAL_MS 3.f       /* fade of a stolen voice */
#endif /* chorus delay line (>= 20 ms at 96 kHz), power of two */

/* ------------------------------------------------------------------ DSP primitives */
static inline float va_clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline int va_clampi(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
static inline float va_tanh(float x) {               /* Pade, exact saturation at |x| >= 3 */
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
/* sin(2 pi ph), ph in [0,1) */
static inline float va_sin(float ph) {
    float u = ph * 2.f - 1.f;
    float y = 4.f * u * (1.f - fabsf(u));
    y = 0.225f * (y * fabsf(y) - y) + y;
    return -y;
}
/* fast 2^x for |x| < 30 (rel. error ~2e-5) */
static inline float va_exp2(float x) {
    float fl = floorf(x), f = x - fl;
    float p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    return ldexpf(p, (int)fl);
}
static inline uint32_t va_rng(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float va_noise(uint32_t *s) { return (float)(int32_t)va_rng(s) * (1.f / 2147483648.f); }
static inline float va_note_hz(float n) { return 440.f * exp2f((n - 69.f) * (1.f / 12.f)); }

/* band-limiting residuals (d = fraction of a sample elapsed since the event, at the sample after it).
   step of height H: after += H * a, before += H * b ; slope change S (per sample): after += S * a, before += S * b */
static inline void va_blep(float d, float H, float *after, float *before) {
    float e = 1.f - d;
    *after -= 0.5f * H * e * e;
    *before += 0.5f * H * d * d;
}
static inline void va_blamp(float d, float S, float *after, float *before) {
    float e = 1.f - d;
    *after += S * e * e * e * (1.f / 6.f);
    *before += S * d * d * d * (1.f / 6.f);
}

/* IR3109-style ladder: 4 TPT one-poles, ZDF linear solve of the feedback, tanh at the OTA input.
   G = g / (1 + g), g = tan(pi fc / sr); k = 0..~4.6 (self-oscillates above 4); D = 1 / (1 + k G^4).
   Returns the 24 dB output; *y2 = 12 dB. */
typedef struct { float s1, s2, s3, s4; } va_lad;
static inline float va_lad_tick(va_lad *f, float x, float G, float k, float D, float *y2o) {
    float iG = 1.f - G, G2 = G * G;
    float S = G2 * G * iG * f->s1 + G2 * iG * f->s2 + G * iG * f->s3 + iG * f->s4;
    float u = (x - k * S) * D;                       /* D = 1 / (1 + k G^4), interpolated per tick */
    u = va_tanh(u);
    float v = (u - f->s1) * G, y1 = v + f->s1; f->s1 = y1 + v;
    v = (y1 - f->s2) * G; float y2 = v + f->s2; f->s2 = y2 + v;
    v = (y2 - f->s3) * G; float y3 = v + f->s3; f->s3 = y3 + v;
    v = (y3 - f->s4) * G; float y4 = v + f->s4; f->s4 = y4 + v;
    *y2o = y2;
    return y4;
}
static inline float va_lad_D(float G, float k) { float G2 = G * G; return 1.f / (1.f + k * G2 * G2); }
static inline float va_lad_G(float fc, float sr) {
    float lim = 0.45f * sr; if (fc > lim) fc = lim; if (fc > 20000.f) fc = 20000.f; if (fc < 8.f) fc = 8.f;
    float g = tanf(VA_PI * fc / sr);
    return g / (1.f + g);
}

/* ------------------------------------------------------------------ parameter description */
enum { F_PCT, F_BIP, F_TATK, F_TDEC, F_TREL, F_LFOHZ, F_CUT, F_ST, F_CENT, F_CHOICE, F_LEVEL, F_PAN, F_AMT, F_INT, F_GLIDE, F_DELAY,
       F_PHASE, F_HPF };
typedef struct {
    const char *page, *name;
    signed char kind, fmt, basic;
    short min, max, def;
    const char *const *ch;           /* choice texts (switch / toggle) */
} va_ps;

/* common block appended to every engine's table */
enum { CO_L1WAVE, CO_L1RATE, CO_L1SYNC, CO_L1DELAY, CO_L1RETRIG, CO_L1PHASE,
       CO_L2WAVE, CO_L2RATE, CO_L2SYNC, CO_L2RETRIG,
       CO_BEND, CO_GLIDE, CO_DRIFT,
       CO_LEVEL, CO_PAN, CO_SPREAD,
       CO_MOD, CO_N = CO_MOD + 24 };

static const char *const VA_LFOW[] = {"SINE", "TRI", "SAW UP", "SAW DN", "SQUARE", "S&H", "SMOOTH RND", "NOISE"};
enum { W_SINE, W_TRI, W_SAWU, W_SAWD, W_SQR, W_SH, W_SMOOTH, W_NOISE };
static const char *const VA_OFFON[] = {"OFF", "ON"};
static const char *const VA_FREEKEY[] = {"FREE", "KEY"};
static const char *const VA_FREESYNC[] = {"FREE", "SYNC"};
/* mod sources */
enum { S_OFF, S_LFO1, S_LFO2, S_FENV, S_AENV, S_VEL, S_NOTE, S_WHEEL, S_AT, S_RND, S_BEND, S_L1WHL, S_N };
static const char *const VA_SRC[S_N] = {"OFF", "LFO 1", "LFO 2", "FILTER ENV", "AMP ENV", "VELOCITY", "NOTE",
                                        "MOD WHEEL", "AFTERTOUCH", "RANDOM", "PITCH BEND", "LFO1*WHEEL"};
/* internal mod destinations (engines expose a subset with their own names). Units for amount 100 at source 1:
   PITCH/OSC1/OSC2 12 st, PW 0.5, levels/xmod/drive/env amt/reso 1.0 (full knob range), CUT 60 st, HPF 48 st,
   env times and LFO rates x16 (4 octaves), SUS 1.0, L1AMT +100 %, LEVEL +100 %, PAN full, FX full */
enum { D_NONE, D_PITCH, D_OSC1, D_OSC2, D_PW, D_LV1, D_LV2, D_LV3, D_LVN, D_XMOD, D_CUT, D_RESO, D_HPF, D_FENV,
       D_DRIVE, D_ATT, D_DEC, D_SUS, D_REL, D_L1RATE, D_L1AMT, D_L2RATE, D_LEVEL, D_PAN, D_FX, D_N };

/* LFO sync divisions (length in beats) */
static const char *const VA_SYNCN[16] = {"4 bars", "2 bars", "1 bar", "1/2 D", "1/2", "1/4 D", "1/2 T", "1/4",
                                         "1/8 D", "1/4 T", "1/8", "1/16 D", "1/8 T", "1/16", "1/16 T", "1/32"};
static const float VA_SYNCB[16] = {16.f, 8.f, 4.f, 3.f, 2.f, 1.5f, 4.f / 3.f, 1.f, 0.75f, 2.f / 3.f, 0.5f, 0.375f,
                                   1.f / 3.f, 0.25f, 1.f / 6.f, 0.125f};

#define VA_SW(pg, nm, ch, def, basic) {pg, nm, JP_SWITCH, F_CHOICE, basic, 0, (short)(sizeof(ch) / sizeof(ch[0]) - 1), def, ch}
#define VA_TG(pg, nm, def, basic) {pg, nm, JP_TOGGLE, F_CHOICE, basic, 0, 1, def, VA_OFFON}
#define VA_KN(pg, nm, fmt, mn, mx, def, basic) {pg, nm, JP_KNOB, fmt, basic, mn, mx, def, NULL}

static const va_ps VA_CPS[CO_N] = {
    VA_SW("LFO 1", "WAVE", VA_LFOW, W_TRI, 1), VA_KN("LFO 1", "RATE", F_LFOHZ, 0, 127, 80, 1),
    VA_SW("LFO 1", "SYNC", VA_FREESYNC, 0, 1), VA_KN("LFO 1", "DELAY", F_DELAY, 0, 127, 0, 1),
    VA_SW("LFO 1", "RETRIG", VA_FREEKEY, 0, 1), VA_KN("LFO 1", "PHASE", F_PHASE, 0, 127, 0, 0),
    VA_SW("LFO 2", "WAVE", VA_LFOW, W_SINE, 0), VA_KN("LFO 2", "RATE", F_LFOHZ, 0, 127, 60, 0),
    VA_SW("LFO 2", "SYNC", VA_FREESYNC, 0, 1), VA_SW("LFO 2", "RETRIG", VA_FREEKEY, 0, 1),
    VA_KN("VOICE", "BEND RANGE", F_ST, 0, 24, 2, 1), VA_KN("VOICE", "GLIDE", F_GLIDE, 0, 127, 0, 1),
    VA_KN("VOICE", "DRIFT", F_PCT, 0, 127, 20, 0),
    VA_KN("LEVEL", "LEVEL", F_LEVEL, 0, 127, 100, 1), VA_KN("LEVEL", "PAN", F_PAN, -64, 63, 0, 0),
    VA_KN("LEVEL", "SPREAD", F_PCT, 0, 127, 0, 0),
#define VA_MODP(n) {"MOD " #n, "SOURCE", JP_SWITCH, F_CHOICE, 0, 0, S_N - 1, 0, VA_SRC}, \
                   {"MOD " #n, "DEST", JP_SWITCH, F_CHOICE, 0, 0, 1, 0, NULL},           \
                   {"MOD " #n, "AMOUNT", JP_KNOB, F_AMT, 0, -100, 100, 0, NULL}
    VA_MODP(1), VA_MODP(2), VA_MODP(3), VA_MODP(4), VA_MODP(5), VA_MODP(6), VA_MODP(7), VA_MODP(8),
#undef VA_MODP
};

/* presets: pairs (param index, value) terminated by -1, applied over the table defaults */
#define VA_PK 96
typedef struct { const char *name, *cat; int flags; const char *arp; short kv[VA_PK]; } va_preset;
/* macros: up to 4 targets with weights (offset = macro * weight * range / 127) */
typedef struct { const char *name; int n; struct { int p; float w; int pos; } t[4]; } va_macro;   /* pos 1 / -1: acts on the + / - half only */

struct va;
typedef struct va_voice va_voice;
enum { VM_POLY, VM_MONO, VM_UNI };

typedef struct va_eng {
    const char *id;
    int npages; const char *const *pages;
    int ns; const va_ps *ps;                  /* engine params (common block follows at index ns) */
    int ndest; const char *const *dname; const unsigned char *did;
    int npre; const va_preset *pre;
    const va_macro *mac;
    const float *trim;                        /* per-preset output trim (generated, see va_trims.h) or NULL */
    int poly;
    int envp[2][4];                           /* ATTACK, DECAY, SUSTAIN, RELEASE param of env 0 (filter/mod) and 1 (amp) */
    int envkey[2];                            /* key follow param per env or -1 */
    int p_vcagate;                            /* VCA ENV/GATE switch or -1 */
    int p_cutoff;                             /* CC74 target */
    int p_envtrig;                            /* KEY/LFO env trigger switch or -1 */
    int p_porta;                              /* OFF/ON/AUTO switch or -1 (then glide is ON whenever GLIDE > 0) */
    int p_legato;                             /* toggle or -1 */
    void (*voicing)(struct va *s, int *mode, int *stack, float *detune, int *poly1);
    void (*von)(struct va *s, va_voice *v);   /* fresh voice: oscillator phases etc. */
    void (*vctrl)(struct va *s, va_voice *v, int fresh);
    void (*vdsp)(struct va *s, va_voice *v, float *out, int n);
    void (*post)(struct va *s, float *L, float *R, int n);
    void (*post_reset)(struct va *s);
} va_eng;

typedef struct { int st; float v; } va_env;     /* st: 0 idle, 1 attack, 2 decay/sustain, 3 release */

struct va_voice {
    int st;                 /* 0 free, 1 playing, 2 fading out (stolen / killed) */
    int key, sus, rel, quick, fresh;
    int note; float vel; unsigned age;
    float pitch, ptarget, gc; /* glide (semitones), glide coefficient per tick */
    float detune, upan, ugain; /* unison offset (st), pan offset, gain */
    float fade, fstep;
    va_env env[2];
    float lph[2], lval[2], lhold[2], lfrom[2], lto[2], lt;  /* LFOs; lt = seconds since note on (LFO delay) */
    float rnd; uint32_t rng;
    float dstat, dwan, dwt; /* drift: static voice offset, wander value, wander target */
    float drift;            /* current drift (-1..1) */
    int cnt;
    float md[D_N];
    float src[S_N];
    float c[VA_NC], ci[VA_NC];  /* control channels: 0, 1 = pan gains, 2.. engine */
    /* engine DSP state */
    float ph[3]; int sc; int flip, first; float pend; va_lad lad; float hp, hpx, o2;
};

typedef struct { int frame, st, d1, d2; } va_ev;

typedef struct va {
    const va_eng *E;
    float sr, isr, nzk;                         /* nzk: noise gain sqrt(sr / 48 kHz), same filtered level at any rate */
    int np; va_ps ps[VA_MAXP]; int pg[VA_MAXP];
    int base[VA_MAXP], eff[VA_MAXP], mac[8];
    float sm[VA_MAXP], smk;
    int cb;                                     /* index of the common block */
    int preset; float trim;
    va_voice v[VA_MAXSLOT];
    unsigned agec;
    int stack[32], nstack;                      /* mono: held keys, newest last */
    int monov[8], nmonov;                       /* mono: the voices of the current note */
    float last_pitch; int have_last;
    int sus;
    float wheel_t, wheel, at_t, at, bend_t, bend;
    float gph[2], glv[2], ghold[2];             /* free-running LFO phases (voices copy them at note on) + values (FX) */
    double bpm, beat0; int playing;
    int gcnt, bpos;
    float ecA[2], ecD[2], ecR[2], esus[2];      /* per-tick envelope coefficients */
    float gcA, gcR;                             /* VCA GATE coefficients (macro ATTACK / RELEASE offsets act on the gate) */
    float lhz[2];                               /* LFO base rates */
    va_ev ev[VA_MAXEV]; int nev;
    float tmp[VA_BLOCK];
    float lev, dcx[2], dcy[2], dcR, lim, limrel;
    uint32_t rng;
    double pk;
    char tb[8][40]; int ti;
    /* AURA post: HPF + BBD chorus */
    struct {
        float hs[2][2], hg, hk, hx, hh, hl, hgt, hxt, hht, hlt;  /* SVF states, smoothed coef + output gains */
        float buf[2][VA_CHBUF]; int w;
        float ph, rate, cen, rng_ms, wet, pre[2][2], post[2], hissz[2], env, nz;
        uint32_t nrng; int idle;
    } ju;
} va;

/* ------------------------------------------------------------------ value mappings (documented in README) */
/* envelope times (J2-5 law, same fader feel in every engine): x = v / 127, t = tmin + (tmax - tmin) * x^4
   stage 0 ATTACK 0.5 ms .. 20 s, 1 DECAY 1 ms .. 20 s, 2 RELEASE 2 ms .. 30 s
   (attack: 10 % = 2.5 ms, 25 % = 78 ms, 50 % = 1.25 s, 75 % = 6.3 s; until J2-5: 0.5 ms * 40000^(v/127) for all three) */
static const float VA_ETMIN[3] = {0.0005f, 0.001f, 0.002f}, VA_ETMAX[3] = {20.f, 20.f, 30.f};
static float va_env_s(int st, float v) {
    float x = va_clampf(v, 0.f, 127.f) * (1.f / 127.f), x2 = x * x;
    return VA_ETMIN[st] + (VA_ETMAX[st] - VA_ETMIN[st]) * x2 * x2;
}
static float va_lfo_hz(float v) { return 0.03f * powf(1000.f, va_clampf(v, 0.f, 127.f) / 127.f); }     /* 0.03 .. 30 Hz */
static float va_delay_s(float v) { float x = v / 127.f; return 5.f * x * x; }                         /* 0 .. 5 s */
static float va_glide_s(float v) { float x = v / 127.f; return 3.f * x * x; }                         /* 0 .. 3 s */
static float va_cut_note(float v) { return 12.f + v * (123.f / 127.f); }                               /* 16 Hz .. 20 kHz */
static float va_level_gain(float v) { float x = v / 100.f; return x * x; }                             /* 100 = 0 dB */
static int va_sync_idx(float v) { return va_clampi((int)(v * 15.f / 127.f + 0.5f), 0, 15); }

/* ------------------------------------------------------------------ params */
static int va_okp(va *s, int i) { return i >= 0 && i < s->np; }
static int va_macro_add(va *s, int i) {
    int add = 0;
    for (int m = 0; m < 8; m++) {
        int mv = s->mac[m]; if (!mv) continue;
        const va_macro *mc = &s->E->mac[m];
        for (int j = 0; j < mc->n; j++) if (mc->t[j].p == i && !(mc->t[j].pos > 0 && mv < 0) && !(mc->t[j].pos < 0 && mv > 0)) {
            float r = (float)(s->ps[i].max - s->ps[i].min);
            add += (int)lrintf((float)mv * mc->t[j].w * r / 127.f);
        }
    }
    return add;
}
static void va_recalc(va *s) {
    for (int i = 0; i < s->np; i++) s->eff[i] = va_clampi(s->base[i] + va_macro_add(s, i), s->ps[i].min, s->ps[i].max);
}
#define PV(s, i) ((s)->sm[i])               /* smoothed (knobs) / exact (switches) value */
#define PI_(s, i) ((s)->eff[i])

static void va_reset(void *e);
static void va_preset_load(void *e, int i);
static void *va_create(const va_eng *E, double sr) {
    if (sr < 8000.0 || sr > 384000.0) return NULL;
    va *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->E = E; s->sr = (float)sr; s->isr = 1.f / s->sr; s->nzk = sqrtf(s->sr / 48000.f);
    s->np = E->ns + CO_N; s->cb = E->ns;
    if (s->np > VA_MAXP) { free(s); return NULL; }
    for (int i = 0; i < E->ns; i++) s->ps[i] = E->ps[i];
    for (int i = 0; i < CO_N; i++) {
        s->ps[E->ns + i] = VA_CPS[i];
        if (i >= CO_MOD && (i - CO_MOD) % 3 == 1) { s->ps[E->ns + i].max = (short)(E->ndest - 1); s->ps[E->ns + i].ch = E->dname; }
    }
    for (int i = 0; i < s->np; i++) {
        s->pg[i] = 0;
        for (int p = 0; p < E->npages; p++) if (!strcmp(E->pages[p], s->ps[i].page)) { s->pg[i] = p; break; }
    }
    s->smk = 1.f - expf(-(float)VA_TICK / (0.008f * s->sr));
    s->dcR = 1.f - 2.f * VA_PI * 6.f / s->sr;
    s->limrel = expf(-1.f / (0.12f * s->sr));
    s->bpm = 120.0;
    va_preset_load(s, 0);
    va_reset(s);
    return s;
}
static void va_destroy(void *e) { free(e); }

static void va_reset(void *e) {
    va *s = e;
    memset(s->v, 0, sizeof s->v);
    for (int k = 0; k < VA_MAXSLOT; k++) {          /* per-slot "voice card" offsets (deterministic) */
        uint32_t r = 0x51ED27u + 977u * (unsigned)k;
        va_rng(&r); va_rng(&r);
        s->v[k].dstat = va_noise(&r);
    }
    s->nev = 0; s->nstack = 0; s->nmonov = 0; s->have_last = 0; s->sus = 0;
    s->wheel = s->wheel_t = s->at = s->at_t = s->bend = s->bend_t = 0.f;
    s->gph[0] = s->gph[1] = 0.f; s->glv[0] = s->glv[1] = 0.f; s->ghold[0] = s->ghold[1] = 0.f;
    s->gcnt = 0; s->agec = 0;
    s->rng = 0x9E3779B9u;
    s->dcx[0] = s->dcx[1] = s->dcy[0] = s->dcy[1] = 0.f; s->lim = 0.f;
    for (int i = 0; i < s->np; i++) s->sm[i] = (float)s->eff[i];
    s->lev = va_level_gain((float)s->eff[s->cb + CO_LEVEL]) * s->trim;
    if (s->E->post_reset) s->E->post_reset(s);
}

/* ------------------------------------------------------------------ global control tick */
static void va_env_coefs(va *s) {
    const va_eng *E = s->E;
    float T = (float)VA_TICK / s->sr;
    for (int k = 0; k < 2; k++) {
        const int *p = E->envp[k];
        float a = va_env_s(0, PV(s, p[0])), d = va_env_s(1, PV(s, p[1])), r = va_env_s(2, PV(s, p[3]));
        s->ecA[k] = expf(-T * 1.4663f / a);
        s->ecD[k] = expf(-T * 9.2103f / d);
        s->ecR[k] = expf(-T * 9.2103f / r);
        s->esus[k] = PV(s, p[2]) / 127.f;
    }
    /* VCA GATE: 1 ms on, 4 ms off; a macro that pushes the ADSR attack / release up (e.g. ATTACK, RELEASE on a GATE
       organ) also lengthens the gate's own edges by the same macro amount (the preset at macro 0 is unchanged) */
    if (E->p_vcagate >= 0) {
        int oa = va_macro_add(s, E->envp[1][0]), orl = va_macro_add(s, E->envp[1][3]);
        float ga = oa > 0 ? fmaxf(0.001f, va_env_s(0, (float)oa)) : 0.001f, gr = orl > 0 ? fmaxf(0.004f, va_env_s(2, (float)orl)) : 0.004f;
        s->gcA = expf(-T * 1.4663f / ga); s->gcR = expf(-T * 9.21f / gr);
    }
}
static float va_lfo_rate(va *s, int l) {
    int cb = s->cb;
    int rp = cb + (l ? CO_L2RATE : CO_L1RATE), sp = cb + (l ? CO_L2SYNC : CO_L1SYNC);
    if (s->eff[sp]) return (float)(s->bpm / 60.0) / VA_SYNCB[va_sync_idx((float)s->eff[rp])];
    return va_lfo_hz(PV(s, rp));
}
static float va_lfo_wave(int w, float ph, float hold, float from, float to) {
    switch (w) {
    case W_SINE: return va_sin(ph);
    case W_TRI: return ph < 0.25f ? 4.f * ph : ph < 0.75f ? 2.f - 4.f * ph : 4.f * ph - 4.f;
    case W_SAWU: return 2.f * ph - 1.f;
    case W_SAWD: return 1.f - 2.f * ph;
    case W_SQR: return ph < 0.5f ? 1.f : -1.f;
    case W_SMOOTH: { float x = ph * ph * (3.f - 2.f * ph); return from + (to - from) * x; }
    default: return hold;          /* S&H, NOISE */
    }
}
static void va_gtick(va *s) {
    for (int i = 0; i < s->np; i++) {
        float t = (float)s->eff[i];
        if (s->ps[i].kind != JP_KNOB) { s->sm[i] = t; continue; }
        float d = t - s->sm[i];
        s->sm[i] = fabsf(d) < 1e-4f ? t : s->sm[i] + d * s->smk;
    }
    float k = s->smk * 1.6f;
    s->wheel += (s->wheel_t - s->wheel) * k;
    s->at += (s->at_t - s->at) * k;
    s->bend += (s->bend_t - s->bend) * k;
    va_env_coefs(s);
    float T = (float)VA_TICK / s->sr;
    for (int l = 0; l < 2; l++) {
        s->lhz[l] = va_lfo_rate(s, l);
        float ph = s->gph[l] + s->lhz[l] * T;
        if (ph >= 1.f) { ph -= (float)(int)ph; s->ghold[l] = va_noise(&s->rng); }
        s->gph[l] = ph;
        int w = s->eff[s->cb + (l ? CO_L2WAVE : CO_L1WAVE)];
        s->glv[l] = va_lfo_wave(w, ph, s->ghold[l], s->ghold[l], s->ghold[l]);
    }
}

/* ------------------------------------------------------------------ voice control tick */
static inline void va_set(va_voice *v, int ch, float target, int jump) {
    if (jump) { v->c[ch] = target; v->ci[ch] = 0.f; }
    else v->ci[ch] = (target - v->c[ch]) * (1.f / VA_TICK);
}
static inline void va_env_tick(va_env *e, float ca, float cd, float sus, float cr) {
    switch (e->st) {
    case 1: e->v = 1.3f + (e->v - 1.3f) * ca; if (e->v >= 1.f) { e->v = 1.f; e->st = 2; } break;
    case 2: e->v = sus + (e->v - sus) * cd; if (e->v < 1e-9f) e->v = 0.f; break;
    case 3: e->v *= cr; if (e->v < 1e-4f) { e->v = 0.f; e->st = 0; } break;
    default: break;
    }
}
static void va_env_trigger(va_voice *v) { v->env[0].st = 1; v->env[1].st = 1; }
static void va_voice_release(va_voice *v) {
    v->rel = 1; v->key = 0; v->sus = 0;
    for (int k = 0; k < 2; k++) if (v->env[k].st) v->env[k].st = 3;
}

/* the mod matrix: sum each slot into md[] */
static void va_mods(va *s, va_voice *v) {
    memset(v->md, 0, sizeof v->md);
    int b = s->cb + CO_MOD;
    for (int m = 0; m < 8; m++) {
        int src = s->eff[b + 3 * m], dst = s->eff[b + 3 * m + 1];
        float amt = PV(s, b + 3 * m + 2);
        if (!src || !dst || amt == 0.f) continue;
        v->md[s->E->did[dst]] += v->src[src] * amt * 0.01f;
    }
}
/* global-source view of the matrix (for global destinations such as FX) */
static float va_gmod(va *s, int dest) {
    int b = s->cb + CO_MOD; float sum = 0.f;
    for (int m = 0; m < 8; m++) {
        int src = s->eff[b + 3 * m], dst = s->eff[b + 3 * m + 1];
        if (!src || !dst || s->E->did[dst] != dest) continue;
        float x = 0.f;
        switch (src) {
        case S_LFO1: x = s->glv[0]; break;
        case S_LFO2: x = s->glv[1]; break;
        case S_WHEEL: x = s->wheel; break;
        case S_AT: x = s->at; break;
        case S_BEND: x = s->bend; break;
        case S_L1WHL: x = s->glv[0] * s->wheel; break;
        default: break;
        }
        sum += x * PV(s, b + 3 * m + 2) * 0.01f;
    }
    return sum;
}

static void va_vtick(va *s, va_voice *v, int pos) {
    const va_eng *E = s->E;
    int cb = s->cb;
    float T = (float)VA_TICK / s->sr;
    /* glide */
    if (v->gc > 0.f && fabsf(v->ptarget - v->pitch) > 1e-4f) v->pitch = v->ptarget + (v->pitch - v->ptarget) * v->gc;
    else v->pitch = v->ptarget;
    /* drift: static voice offset + slow wander */
    v->dwt -= T;
    if (v->dwt <= 0.f) { v->dwt = 0.6f + 0.8f * (0.5f + 0.5f * va_noise(&v->rng)); v->dwan = va_noise(&v->rng); }
    v->drift += (0.6f * v->dstat + 0.4f * v->dwan - v->drift) * (T * 1.5f);
    /* LFOs */
    v->lt += T;
    for (int l = 0; l < 2; l++) {
        float hz = s->lhz[l];
        float m = v->md[l ? D_L2RATE : D_L1RATE];
        if (m != 0.f) hz *= exp2f(va_clampf(m * 4.f, -8.f, 8.f));
        int sync = s->eff[cb + (l ? CO_L2SYNC : CO_L1SYNC)], retrig = s->eff[cb + (l ? CO_L2RETRIG : CO_L1RETRIG)];
        float ph = v->lph[l], np;
        int wrapped = 0;
        if (sync && s->playing && !retrig) {
            double beat = s->beat0 + (double)pos * s->bpm / (60.0 * s->sr);
            double cyc = beat / VA_SYNCB[va_sync_idx((float)s->eff[cb + (l ? CO_L2RATE : CO_L1RATE)])];
            float off = l ? 0.f : (float)s->eff[cb + CO_L1PHASE] / 128.f;
            np = (float)(cyc - floor(cyc)) + off; np -= (float)(int)np;
            wrapped = np < ph;
        } else {
            np = ph + hz * T;
            if (np >= 1.f) { np -= (float)(int)np; wrapped = 1; }
        }
        v->lph[l] = np;
        int w = s->eff[cb + (l ? CO_L2WAVE : CO_L1WAVE)];
        if (wrapped) { v->lhold[l] = va_noise(&v->rng); v->lfrom[l] = v->lto[l]; v->lto[l] = v->lhold[l]; }
        if (w == W_NOISE) v->lhold[l] = va_noise(&v->rng);
        float val = va_lfo_wave(w, np, v->lhold[l], v->lfrom[l], v->lto[l]);
        if (!l) {
            float d = va_delay_s(PV(s, cb + CO_L1DELAY));
            if (d > 0.f) val *= va_clampf((v->lt - d) / (0.5f * d + 0.002f), 0.f, 1.f);
            if (wrapped && E->p_envtrig >= 0 && s->eff[E->p_envtrig] && v->key) va_env_trigger(v);  /* env LFO trigger */
        }
        v->lval[l] = val;
    }
    /* envelopes (time mods / key follow per voice) */
    for (int k = 0; k < 2; k++) {
        float ca = s->ecA[k], cd = s->ecD[k], cr = s->ecR[k], su = s->esus[k];
        float f = 0.f;
        if (E->envkey[k] >= 0) f -= PV(s, E->envkey[k]) / 127.f * (float)(v->note - 60) / 24.f;
        float fa = f + v->md[D_ATT] * 4.f, fd = f + v->md[D_DEC] * 4.f, fr = f + v->md[D_REL] * 4.f;
        if (fa != 0.f) ca = powf(ca, exp2f(-va_clampf(fa, -10.f, 10.f)));
        if (fd != 0.f) cd = powf(cd, exp2f(-va_clampf(fd, -10.f, 10.f)));
        if (fr != 0.f) cr = powf(cr, exp2f(-va_clampf(fr, -10.f, 10.f)));
        if (v->md[D_SUS] != 0.f) su = va_clampf(su + v->md[D_SUS], 0.f, 1.f);
        if (k == 1 && E->p_vcagate >= 0 && s->eff[E->p_vcagate]) {      /* VCA GATE: 1 ms on, 3 ms off */
            ca = s->gcA; cd = 0.f; su = 1.f; cr = s->gcR;
        }
        if (v->quick) { float q = expf(-T * 9.21f / 0.15f); if (cr > q) cr = q; }
        va_env_tick(&v->env[k], ca, cd, su, cr);
    }
    /* sources */
    float *S = v->src;
    S[S_OFF] = 0.f; S[S_LFO1] = v->lval[0]; S[S_LFO2] = v->lval[1];
    S[S_FENV] = v->env[0].v; S[S_AENV] = v->env[1].v; S[S_VEL] = v->vel;
    S[S_NOTE] = (float)(v->note - 60) / 60.f; S[S_WHEEL] = s->wheel; S[S_AT] = s->at; S[S_RND] = v->rnd;
    S[S_BEND] = s->bend; S[S_L1WHL] = v->lval[0] * s->wheel;
    va_mods(s, v);
    /* pan gains (balance law: centre = 1 / 1) */
    float pan = PV(s, cb + CO_PAN) / 64.f + v->upan + v->md[D_PAN];
    pan = va_clampf(pan, -1.f, 1.f);
    va_set(v, 0, pan > 0.f ? 1.f - pan : 1.f, v->fresh);
    va_set(v, 1, pan < 0.f ? 1.f + pan : 1.f, v->fresh);
}

/* ------------------------------------------------------------------ voice allocation */
static int va_nactive(va *s) { int n = 0; for (int k = 0; k < VA_MAXSLOT; k++) if (s->v[k].st == 1) n++; return n; }
static void va_kill(va *s, va_voice *v, float ms) {
    if (v->st != 1) return;
    v->st = 2; v->fade = 1.f; v->fstep = 1.f / (ms * 0.001f * s->sr);
    v->key = 0; v->sus = 0;
}
static va_voice *va_free_slot(va *s) {
    for (int k = 0; k < VA_MAXSLOT; k++) if (s->v[k].st == 0) return &s->v[k];
    va_voice *b = NULL;                                   /* all slots busy: end the most faded voice now */
    for (int k = 0; k < VA_MAXSLOT; k++) if (s->v[k].st == 2 && (!b || s->v[k].fade < b->fade)) b = &s->v[k];
    if (!b) b = &s->v[0];
    b->st = 0;
    return b;
}
/* a slot for a new voice; steals (fade 3 ms) when the polyphony limit is reached */
static va_voice *va_alloc(va *s, int limit) {
    if (va_nactive(s) >= limit) {
        va_voice *vic = NULL;
        for (int k = 0; k < VA_MAXSLOT; k++) {                   /* quietest released voice */
            va_voice *v = &s->v[k];
            if (v->st == 1 && v->rel && (!vic || v->env[1].v < vic->env[1].v)) vic = v;
        }
        if (!vic) for (int k = 0; k < VA_MAXSLOT; k++) {          /* oldest held (or pedal-held) voice */
            va_voice *v = &s->v[k];
            if (v->st == 1 && (!vic || v->age < vic->age)) vic = v;
        }
        if (vic) {
            for (int j = 0; j < s->nmonov; j++) if (&s->v[s->monov[j]] == vic) s->monov[j] = -1;
            va_kill(s, vic, VA_STEAL_MS);
        }
    }
    return va_free_slot(s);
}
static float va_glide_coef(va *s) {
    float t = va_glide_s(PV(s, s->cb + CO_GLIDE));
    if (t <= 0.0005f) return 0.f;
    return expf(-(float)VA_TICK / (s->sr * t / 3.f));
}
static void va_start(va *s, va_voice *v, int note, int vel, float from_pitch, int glide, float det, float upan, float ugain) {
    float dstat = v->dstat;
    memset(v, 0, sizeof *v);
    v->dstat = dstat;
    v->st = 1; v->key = 1; v->note = note; v->vel = (float)vel / 127.f; v->age = ++s->agec;
    v->rng = va_rng(&s->rng) | 1u;
    v->rnd = va_noise(&v->rng);
    v->dwan = va_noise(&v->rng); v->drift = 0.6f * dstat + 0.4f * v->dwan; v->dwt = 0.5f;
    v->detune = det; v->upan = upan; v->ugain = ugain;
    v->ptarget = (float)note;
    v->gc = glide ? va_glide_coef(s) : 0.f;
    v->pitch = glide ? from_pitch : (float)note;
    int cb = s->cb;
    for (int l = 0; l < 2; l++) {
        int retrig = s->eff[cb + (l ? CO_L2RETRIG : CO_L1RETRIG)];
        v->lph[l] = retrig ? (l ? 0.f : (float)s->eff[cb + CO_L1PHASE] / 128.f) : s->gph[l];
        v->lhold[l] = v->lto[l] = s->ghold[l]; v->lfrom[l] = s->ghold[l];
    }
    va_env_trigger(v);
    v->c[0] = v->c[1] = 1.f; v->fresh = 1; v->first = 1;
    s->E->von(s, v);
    v->cnt = 0;
}
static void va_voicing(va *s, int *mode, int *stack, float *det, int *poly1) {
    *mode = VM_POLY; *stack = 1; *det = 0.f; *poly1 = 0;
    if (s->E->voicing) s->E->voicing(s, mode, stack, det, poly1);
}
static int va_glide_on(va *s, int overlap) {
    if (PV(s, s->cb + CO_GLIDE) <= 0.f) return 0;
    if (s->E->p_porta < 0) return 1;
    int m = s->eff[s->E->p_porta];
    return m == 1 || (m == 2 && overlap);
}
static int va_legato(va *s) { return s->E->p_legato >= 0 && s->eff[s->E->p_legato]; }

static void va_mono_start(va *s, int note, int vel, int overlap) {
    int mode, stack, p1; float det;
    va_voicing(s, &mode, &stack, &det, &p1);
    int g = va_glide_on(s, overlap) && s->have_last;
    s->nmonov = 0;
    for (int k = 0; k < stack; k++) {
        va_voice *v = va_alloc(s, s->E->poly);
        float x = stack > 1 ? (float)k / (float)(stack - 1) * 2.f - 1.f : 0.f;
        float spread = PV(s, s->cb + CO_SPREAD) / 127.f;
        va_start(s, v, note, vel, s->last_pitch, g, x * det, x * (stack > 1 ? 0.3f + 0.7f * spread : 0.f),
                 1.f / sqrtf((float)stack));
        s->monov[s->nmonov++] = (int)(v - s->v);
    }
}
static void va_mono_retarget(va *s, int note, int vel, int newkey) {
    int g = va_glide_on(s, 1);
    int retrig = !va_legato(s);
    float gc = g ? va_glide_coef(s) : 0.f;
    for (int j = 0; j < s->nmonov; j++) {
        if (s->monov[j] < 0) continue;
        va_voice *v = &s->v[s->monov[j]];
        if (v->st != 1) continue;
        v->ptarget = (float)note; v->note = note; v->gc = gc;
        if (!g) v->pitch = (float)note;
        if (newkey) v->vel = (float)vel / 127.f;
        v->key = 1; v->rel = 0; v->sus = 0;
        if (retrig) va_env_trigger(v);
    }
}
static int va_mono_alive(va *s) {
    for (int j = 0; j < s->nmonov; j++) if (s->monov[j] >= 0 && s->v[s->monov[j]].st == 1 && !s->v[s->monov[j]].rel) return 1;
    return 0;
}

static void va_note_on(va *s, int note, int vel) {
    int mode, stack, p1; float det;
    va_voicing(s, &mode, &stack, &det, &p1);
    if (mode != VM_POLY) {
        int held = s->nstack > 0 && va_mono_alive(s);
        for (int i = 0; i < s->nstack; i++) if (s->stack[i] == note) { memmove(s->stack + i, s->stack + i + 1, sizeof(int) * (s->nstack - i - 1)); s->nstack--; break; }
        if (s->nstack < 32) s->stack[s->nstack++] = note;
        if (held) va_mono_retarget(s, note, vel, 1);
        else va_mono_start(s, note, vel, 0);
        s->last_pitch = (float)note; s->have_last = 1;
        return;
    }
    /* poly: a key still held retriggers its own voice */
    for (int k = 0; k < VA_MAXSLOT; k++) {
        va_voice *v = &s->v[k];
        if (v->st == 1 && v->key && v->note == note) {
            v->vel = (float)vel / 127.f; va_env_trigger(v);
            s->last_pitch = (float)note; s->have_last = 1;
            return;
        }
    }
    if (p1) for (int k = 0; k < VA_MAXSLOT; k++) if (s->v[k].st == 1 && s->v[k].rel) s->v[k].quick = 1;
    va_voice *v = va_alloc(s, s->E->poly);
    float x = va_noise(&s->rng);
    va_start(s, v, note, vel, s->last_pitch, va_glide_on(s, 0) && s->have_last, 0.f,
             x * PV(s, s->cb + CO_SPREAD) / 127.f, 1.f);
    s->last_pitch = (float)note; s->have_last = 1;
}
static void va_note_off(va *s, int note) {
    int mode, stack, p1; float det;
    va_voicing(s, &mode, &stack, &det, &p1);
    if (mode != VM_POLY) {
        int top = s->nstack ? s->stack[s->nstack - 1] : -1;
        for (int i = 0; i < s->nstack; i++) if (s->stack[i] == note) { memmove(s->stack + i, s->stack + i + 1, sizeof(int) * (s->nstack - i - 1)); s->nstack--; break; }
        if (s->nstack > 0) {
            if (note == top) { int nt = s->stack[s->nstack - 1]; va_mono_retarget(s, nt, 0, 0); s->last_pitch = (float)nt; }
            for (int k = 0; k < VA_MAXSLOT; k++) {
                va_voice *v = &s->v[k];
                int cur = 0;
                for (int j = 0; j < s->nmonov; j++) if (s->monov[j] == k) cur = 1;
                if (!cur && v->st == 1 && v->key && v->note == note) va_voice_release(v);
            }
            return;
        }
        for (int j = 0; j < s->nmonov; j++) {
            if (s->monov[j] < 0) continue;
            va_voice *v = &s->v[s->monov[j]];
            if (v->st != 1 || v->rel || v->note != note) continue;
            if (s->sus) { v->key = 0; v->sus = 1; } else va_voice_release(v);
        }
        /* voices left from a poly passage (VOICES switched while keys were down) */
        for (int k = 0; k < VA_MAXSLOT; k++) {
            va_voice *v = &s->v[k];
            int cur = 0;
            for (int j = 0; j < s->nmonov; j++) if (s->monov[j] == k) cur = 1;
            if (!cur && v->st == 1 && v->key && v->note == note) va_voice_release(v);
        }
        return;
    }
    for (int k = 0; k < VA_MAXSLOT; k++) {
        va_voice *v = &s->v[k];
        if (v->st == 1 && v->key && v->note == note) {
            if (s->sus) { v->key = 0; v->sus = 1; } else va_voice_release(v);
        }
    }
}
static void va_all_off(va *s, int hard) {
    s->nstack = 0; s->sus = 0;
    for (int k = 0; k < VA_MAXSLOT; k++) {
        va_voice *v = &s->v[k];
        if (v->st != 1) continue;
        if (hard) va_kill(s, v, 5.f); else if (!v->rel) va_voice_release(v);
    }
    if (hard) s->nmonov = 0;
}

/* ------------------------------------------------------------------ MIDI */
static void va_param_set(void *e, int i, int v);
static void va_handle(va *s, const va_ev *ev) {
    int st = ev->st & 0xF0, d1 = ev->d1, d2 = ev->d2;
    switch (st) {
    case 0x90: if (d2 > 0) { va_note_on(s, d1, d2); break; } /* fallthrough */
    case 0x80: va_note_off(s, d1); break;
    case 0xB0:
        if (d1 == 1) s->wheel_t = (float)d2 / 127.f;
        else if (d1 == 64) {
            int on = d2 >= 64;
            if (s->sus && !on) {
                for (int k = 0; k < VA_MAXSLOT; k++) if (s->v[k].st == 1 && s->v[k].sus) va_voice_release(&s->v[k]);
            }
            s->sus = on;
        } else if (d1 == 74 && s->E->p_cutoff >= 0) va_param_set(s, s->E->p_cutoff, d2);
        else if (d1 == 123) va_all_off(s, 0);
        else if (d1 == 120) va_all_off(s, 1);
        break;
    case 0xD0: s->at_t = (float)d1 / 127.f; break;
    case 0xE0: { int b = (d2 << 7) | d1; s->bend_t = va_clampf((float)(b - 8192) / 8191.f, -1.f, 1.f); break; }
    default: break;
    }
}
static void va_midi(void *e, int frame, int st, int d1, int d2) {
    va *s = e;
    if (s->nev >= VA_MAXEV) return;
    int t = st & 0xF0;
    if (t != 0x80 && t != 0x90 && t != 0xB0 && t != 0xD0 && t != 0xE0) return;
    if (d1 < 0 || d1 > 127 || d2 < 0 || d2 > 127) return;
    if (frame < 0) frame = 0;
    if (frame > VA_BLOCK - 1) frame = VA_BLOCK - 1;
    s->ev[s->nev++] = (va_ev){frame, st, d1, d2};
}
static void va_tempo(void *e, double bpm, int playing, double beat) {
    va *s = e;
    if (bpm > 1.0 && bpm < 1000.0) s->bpm = bpm;
    s->playing = playing; s->beat0 = beat;
}

/* ------------------------------------------------------------------ render */
static void va_voice_run(va *s, va_voice *v, float *L, float *R, int pos, int n) {
    float *o = s->tmp;
    int off = 0;
    while (off < n) {
        if (v->cnt == 0) {
            va_vtick(s, v, pos + off);
            if (v->st == 1 && v->rel && v->env[1].st == 0) { v->st = 0; return; }
            s->E->vctrl(s, v, v->fresh);
            v->fresh = 0;
            v->cnt = VA_TICK;
        }
        int m = n - off < v->cnt ? n - off : v->cnt;
        s->E->vdsp(s, v, o, m);
        float gl = v->c[0], gr = v->c[1], il = v->ci[0], ir = v->ci[1];
        float *l = L + pos + off, *r = R + pos + off;
        if (v->st == 2) {
            float f = v->fade, fs = v->fstep;
            for (int i = 0; i < m; i++) {
                gl += il; gr += ir;
                f -= fs; if (f <= 0.f) { f = 0.f; v->st = 0; break; }
                float x = o[i] * f;
                l[i] += x * gl; r[i] += x * gr;
            }
            v->fade = f;
        } else {
            for (int i = 0; i < m; i++) { gl += il; gr += ir; l[i] += o[i] * gl; r[i] += o[i] * gr; }
        }
        v->c[0] = gl; v->c[1] = gr;
        v->cnt -= m; off += m;
        if (v->st == 0) return;
    }
}
static void va_run(va *s, float *L, float *R, int pos, int n) {
    while (n > 0) {
        if (s->gcnt == 0) { va_gtick(s); s->gcnt = VA_TICK; }
        int k = n < s->gcnt ? n : s->gcnt;
        for (int j = 0; j < VA_MAXSLOT; j++) if (s->v[j].st) va_voice_run(s, &s->v[j], L, R, pos, k);
        s->gcnt -= k; pos += k; n -= k;
    }
}
static void va_render(void *e, float *L, float *R, int n) {
    va *s = e;
    if (n > VA_BLOCK) n = VA_BLOCK;
    if (n <= 0) return;
    memset(L, 0, sizeof(float) * n); memset(R, 0, sizeof(float) * n);
    for (int i = 1; i < s->nev; i++) {                 /* stable sort by frame */
        va_ev x = s->ev[i]; int j = i - 1;
        while (j >= 0 && s->ev[j].frame > x.frame) { s->ev[j + 1] = s->ev[j]; j--; }
        s->ev[j + 1] = x;
    }
    int pos = 0;
    for (int i = 0; i < s->nev; i++) {
        int f = s->ev[i].frame; if (f >= n) f = n - 1;
        if (f > pos) { va_run(s, L, R, pos, f - pos); pos = f; }
        va_handle(s, &s->ev[i]);
    }
    s->nev = 0;
    if (pos < n) va_run(s, L, R, pos, n - pos);
    if (s->E->post) s->E->post(s, L, R, n);
    /* trim * level (per-sample one pole), DC block, limiter, soft clip */
    float lt = va_level_gain((float)s->eff[s->cb + CO_LEVEL]) * s->trim, lv = s->lev, lk = 1.f - expf(-1.f / (0.01f * s->sr));
    float R0 = s->dcR, lim = s->lim, rel = s->limrel, pk = 0.f;
    float x1l = s->dcx[0], y1l = s->dcy[0], x1r = s->dcx[1], y1r = s->dcy[1];
    for (int i = 0; i < n; i++) {
        lv += (lt - lv) * lk;
        float a = L[i] * lv, b = R[i] * lv;
        float yl = a - x1l + R0 * y1l; x1l = a; y1l = yl;
        float yr = b - x1r + R0 * y1r; x1r = b; y1r = yr;
        float m = fabsf(yl) > fabsf(yr) ? fabsf(yl) : fabsf(yr);
        lim = m > lim ? m : lim * rel;
        if (lim > 0.8f) { float g = 0.8f / lim; yl *= g; yr *= g; }
        float al = fabsf(yl), ar = fabsf(yr);
        if (al > 0.7f) { float o = 0.7f + 0.19f * va_tanh((al - 0.7f) / 0.19f); yl = yl < 0.f ? -o : o; al = o; }
        if (ar > 0.7f) { float o = 0.7f + 0.19f * va_tanh((ar - 0.7f) / 0.19f); yr = yr < 0.f ? -o : o; ar = o; }
        L[i] = yl; R[i] = yr;
        if (al > pk) pk = al;
        if (ar > pk) pk = ar;
    }
    if (fabsf(x1l) < 1e-20f) x1l = 0.f;
    if (fabsf(y1l) < 1e-20f) y1l = 0.f;
    if (fabsf(x1r) < 1e-20f) x1r = 0.f;
    if (fabsf(y1r) < 1e-20f) y1r = 0.f;
    if (lim < 1e-20f) lim = 0.f;
    s->dcx[0] = x1l; s->dcy[0] = y1l; s->dcx[1] = x1r; s->dcy[1] = y1r; s->lim = lim; s->lev = lv;
    s->pk = pk;
}

/* ------------------------------------------------------------------ jeng: parameters */
static int va_npages(void *e) { return ((va *)e)->E->npages; }
static const char *va_page_name(void *e, int p) { va *s = e; return (p >= 0 && p < s->E->npages) ? s->E->pages[p] : ""; }
static int va_nparams(void *e) { return ((va *)e)->np; }
static int va_param_page(void *e, int i) { va *s = e; return va_okp(s, i) ? s->pg[i] : 0; }
static const char *va_param_name(void *e, int i) { va *s = e; return va_okp(s, i) ? s->ps[i].name : ""; }
static int va_param_kind(void *e, int i) { va *s = e; return va_okp(s, i) ? s->ps[i].kind : JP_KNOB; }
static int va_param_min(void *e, int i) { va *s = e; return va_okp(s, i) ? s->ps[i].min : 0; }
static int va_param_max(void *e, int i) { va *s = e; return va_okp(s, i) ? s->ps[i].max : 0; }
static int va_param_basic(void *e, int i) { va *s = e; return va_okp(s, i) ? s->ps[i].basic : 0; }
static int va_param_get(void *e, int i) { va *s = e; return va_okp(s, i) ? s->eff[i] : 0; }
static void va_param_set(void *e, int i, int v) {
    va *s = e; if (!va_okp(s, i)) return;
    v = va_clampi(v, s->ps[i].min, s->ps[i].max);
    s->base[i] = v - va_macro_add(s, i);
    s->eff[i] = va_clampi(s->base[i] + va_macro_add(s, i), s->ps[i].min, s->ps[i].max);
}
/* "0.5 ms", "80.6 ms", "352 ms", "1.25 s", "6.3 s", "20 s" (trailing zeros of seconds dropped) */
static void va_fmt_time(char *b, float t) {
    if (t < 0.09995f) snprintf(b, 40, "%.1f ms", t * 1000.f);
    else if (t < 0.9995f) snprintf(b, 40, "%d ms", (int)(t * 1000.f + 0.5f));
    else {
        snprintf(b, 40, t < 9.995f ? "%.2f" : "%.1f", t);
        char *d = strchr(b, '.');
        if (d) { char *z = b + strlen(b) - 1; while (z > d && *z == '0') *z-- = 0; if (z == d) *z = 0; }
        strcat(b, " s");
    }
}
static void va_fmt_hz(char *b, float f) {
    if (f >= 1000.f) snprintf(b, 40, "%.1f kHz", f / 1000.f);
    else if (f >= 10.f) snprintf(b, 40, "%d Hz", (int)(f + 0.5f));
    else if (f >= 1.f) snprintf(b, 40, "%.2f Hz", f);
    else snprintf(b, 40, "%.3f Hz", f);
}
static const char *va_param_text(void *e, int i, int v) {
    va *s = e;
    char *b = s->tb[s->ti]; s->ti = (s->ti + 1) & 7;
    b[0] = 0;
    if (!va_okp(s, i)) return b;
    const va_ps *p = &s->ps[i];
    v = va_clampi(v, p->min, p->max);
    int cb = s->cb;
    switch (p->fmt) {
    case F_CHOICE: snprintf(b, 40, "%s", p->ch ? p->ch[v - p->min] : "?"); break;
    case F_PCT: snprintf(b, 40, "%d %%", (v * 100 + 63) / 127); break;
    case F_BIP: snprintf(b, 40, v ? "%+d %%" : "%d %%", v * 100 / 64); break;
    case F_TATK: case F_TDEC: case F_TREL: va_fmt_time(b, va_env_s(p->fmt - F_TATK, (float)v)); break;
    case F_DELAY: if (v == 0) snprintf(b, 40, "OFF"); else va_fmt_time(b, va_delay_s((float)v)); break;
    case F_GLIDE: if (v == 0) snprintf(b, 40, "OFF"); else va_fmt_time(b, va_glide_s((float)v)); break;
    case F_LFOHZ: {
        int sp = (i == cb + CO_L2RATE) ? cb + CO_L2SYNC : cb + CO_L1SYNC;
        if (s->eff[sp]) snprintf(b, 40, "%s", VA_SYNCN[va_sync_idx((float)v)]);
        else va_fmt_hz(b, va_lfo_hz((float)v));
        break;
    }
    case F_CUT: va_fmt_hz(b, va_note_hz(va_cut_note((float)v))); break;
    case F_HPF: if (v == 0) snprintf(b, 40, "OFF"); else va_fmt_hz(b, 8.f * exp2f((float)v / 127.f * 7.5f)); break;
    case F_ST: snprintf(b, 40, v ? "%+d st" : "%d st", v); if (i == cb + CO_BEND) snprintf(b, 40, "%d st", v); break;
    case F_CENT: snprintf(b, 40, v ? "%+d ct" : "%d ct", (v * 50) / 64); break;
    case F_LEVEL: if (v == 0) snprintf(b, 40, "-inf dB"); else snprintf(b, 40, "%+.1f dB", 20.f * log10f(va_level_gain((float)v))); break;
    case F_PAN: if (v == 0) snprintf(b, 40, "C"); else snprintf(b, 40, "%c %d", v < 0 ? 'L' : 'R', v < 0 ? (-v * 100 + 32) / 64 : (v * 100 + 31) / 63); break;
    case F_AMT: snprintf(b, 40, v ? "%+d" : "%d", v); break;
    case F_PHASE: snprintf(b, 40, "%d deg", v * 360 / 128); break;
    default: snprintf(b, 40, "%d", v); break;
    }
    return b;
}

/* ------------------------------------------------------------------ macros, presets */
static int va_nmacros(void *e) { (void)e; return 8; }
static const char *va_macro_name(void *e, int m) { va *s = e; return (m >= 0 && m < 8) ? s->E->mac[m].name : ""; }
static void va_macro_set(void *e, int m, int v) {
    va *s = e; if (m < 0 || m >= 8) return;
    s->mac[m] = va_clampi(v, -64, 63); va_recalc(s);
}
static int va_npresets(void *e) { return ((va *)e)->E->npre; }
static const char *va_preset_name(void *e, int i) { va *s = e; return (i >= 0 && i < s->E->npre) ? s->E->pre[i].name : ""; }
static const char *va_preset_cat(void *e, int i) { va *s = e; return (i >= 0 && i < s->E->npre) ? s->E->pre[i].cat : ""; }
static int va_preset_flags(void *e, int i) { va *s = e; return (i >= 0 && i < s->E->npre) ? s->E->pre[i].flags : 0; }
static const char *va_preset_arp(void *e, int i) {
    va *s = e; return (i >= 0 && i < s->E->npre && (s->E->pre[i].flags & JF_ARP)) ? s->E->pre[i].arp : NULL;
}
/* sets every parameter (table defaults, then the preset's pairs); macro offsets are kept (the core owns the knobs) */
static void va_preset_load(void *e, int i) {
    va *s = e;
    if (i < 0 || i >= s->E->npre) return;
    for (int p = 0; p < s->np; p++) s->base[p] = s->ps[p].def;
    s->base[s->cb + CO_MOD + 0] = S_L1WHL; s->base[s->cb + CO_MOD + 1] = 1; s->base[s->cb + CO_MOD + 2] = 5; /* wheel vibrato */
    const short *kv = s->E->pre[i].kv;
    for (int j = 0; j + 1 < VA_PK && kv[j] >= 0; j += 2) if (kv[j] < s->np) s->base[kv[j]] = kv[j + 1];
    for (int p = 0; p < s->np; p++) s->base[p] = va_clampi(s->base[p], s->ps[p].min, s->ps[p].max);
    s->preset = i;
    s->trim = s->E->trim ? s->E->trim[i] : 1.f;
    va_recalc(s);
    /* nothing sounding: no need to glide the smoothed values from the old preset (and no click is possible) */
    int busy = 0;
    for (int k = 0; k < VA_MAXSLOT; k++) if (s->v[k].st) busy = 1;
    if (!busy) {
        for (int p = 0; p < s->np; p++) s->sm[p] = (float)s->eff[p];
        s->lev = va_level_gain((float)s->eff[s->cb + CO_LEVEL]) * s->trim;
        if (s->E->post_reset) s->E->post_reset(s);
    }
}
static int va_nlanes(void *e) { (void)e; return 0; }
static const char *va_lane_name(void *e, int l) { (void)e; (void)l; return ""; }
static int va_lane_note(void *e, int l) { (void)e; (void)l; return -1; }
static double va_peak(void *e) { return ((va *)e)->pk; }

/* ------------------------------------------------------------------ live envelope / LFO state (jeng viz)
   Read-only view of the voice the Sound page follows: the newest held voice (key or pedal), else the newest one still
   releasing (or fading after a steal), else none (envelopes IDLE / 0, LFOs = the free-running global LFOs that the
   next FREE voice copies). Envelope law (va_env_tick): attack v = 1.3 + (v - 1.3) * ca (RC to 1.3, ends at 1.0),
   decay v = sus + (v - sus) * cd, release v *= cr: all three RC = JV_EXP. Stage 2 is decay until v is within 1 % of
   the sustain level, then SUSTAIN. AURA / SOLO have one ADSR page (AMP ENV) feeding VCF and VCA: env[1] (VCA) is
   reported, or env[0] (the ADSR itself, still driving the VCF) when VCA = GATE. TITAN: FILTER ENV = env[0],
   AMP ENV = env[1]. No state is written. */
static va_voice *va_viz_voice(va *s) {
    va_voice *held = NULL, *rel = NULL;
    for (int k = 0; k < VA_MAXSLOT; k++) {
        va_voice *v = &s->v[k];
        if (v->st == 1 && !v->rel) { if (!held || v->age > held->age) held = v; }
        else if (v->st) { if (!rel || v->age > rel->age) rel = v; }
    }
    return held ? held : rel;
}
static void va_viz(void *e, jviz_t *z) {
    va *s = e;
    const va_eng *E = s->E;
    va_voice *v = va_viz_voice(s);
    int cb = s->cb;
    int gate = E->p_vcagate >= 0 && s->eff[E->p_vcagate];
    int shared = E->envp[0][0] == E->envp[1][0];        /* one ADSR page for VCF + VCA */
    int n = 0;
    for (int k = 0; k < 2 && n < JV_MAX; k++) {
        if (shared && k != (gate ? 0 : 1)) continue;
        float su = s->esus[k];
        if (v && v->md[D_SUS] != 0.f) su = va_clampf(su + v->md[D_SUS], 0.f, 1.f);
        if (k == 1 && gate) su = 1.f;
        int st = JV_IDLE; float lv = 0.f;
        if (v) {
            const va_env *en = &v->env[k];
            lv = va_clampf(en->v, 0.f, 1.f);
            switch (en->st) {
            case 1: st = JV_ATTACK; break;
            case 2: st = fabsf(en->v - su) <= 0.01f ? JV_SUSTAIN : JV_DECAY; break;
            case 3: st = JV_RELEASE; break;
            default: st = JV_IDLE; lv = 0.f; break;
            }
        }
        z->env[n].page = s->pg[E->envp[k][0]];
        z->env[n].stage = st; z->env[n].level = lv; z->env[n].sus = su;
        z->env[n].alaw = JV_EXP; z->env[n].dlaw = JV_EXP; z->env[n].rlaw = JV_EXP;
        n++;
    }
    z->nenv = n;
    for (int l = 0; l < 2; l++) {
        z->lfo[l].page = s->pg[cb + (l ? CO_L2WAVE : CO_L1WAVE)];
        if (v) {
            float hz = s->lhz[l];
            int sync = s->eff[cb + (l ? CO_L2SYNC : CO_L1SYNC)], retrig = s->eff[cb + (l ? CO_L2RETRIG : CO_L1RETRIG)];
            float m = v->md[l ? D_L2RATE : D_L1RATE];
            if (!(sync && s->playing && !retrig) && m != 0.f) hz *= exp2f(va_clampf(m * 4.f, -8.f, 8.f));
            z->lfo[l].value = va_clampf(v->lval[l], -1.f, 1.f);
            z->lfo[l].phase = v->lph[l];
            z->lfo[l].hz = hz;
        } else {
            z->lfo[l].value = va_clampf(s->glv[l], -1.f, 1.f);
            z->lfo[l].phase = s->gph[l];
            z->lfo[l].hz = s->lhz[l];
        }
    }
    z->nlfo = 2;
}

#define VA_JENG_FIELDS(createfn)                                                                                       \
    .kind = JE_SYNTH, .create = createfn, .destroy = va_destroy, .reset = va_reset, .midi = va_midi,                  \
    .render = va_render, .tempo = va_tempo, .npages = va_npages, .page_name = va_page_name, .nparams = va_nparams,    \
    .param_page = va_param_page, .param_name = va_param_name, .param_kind = va_param_kind, .param_min = va_param_min, \
    .param_max = va_param_max, .param_get = va_param_get, .param_set = va_param_set, .param_text = va_param_text,     \
    .param_basic = va_param_basic, .nmacros = va_nmacros, .macro_name = va_macro_name, .macro_set = va_macro_set,    \
    .npresets = va_npresets, .preset_name = va_preset_name, .preset_cat = va_preset_cat,                             \
    .preset_flags = va_preset_flags, .preset_load = va_preset_load, .preset_arp = va_preset_arp,                     \
    .nlanes = va_nlanes, .lane_name = va_lane_name, .lane_note = va_lane_note, .peak = va_peak, .viz = va_viz

/* common-block parameter indices for engine tables (NS = number of engine params) */
#define VA_C(NS, x) ((NS) + (x))
#define VA_MOD(NS, slot, field) ((NS) + CO_MOD + 3 * ((slot) - 1) + (field))
#endif
