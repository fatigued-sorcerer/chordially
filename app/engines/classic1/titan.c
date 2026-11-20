// SPDX-License-Identifier: GPL-3.0-only
// Jammer classic1: TITAN, a big classic polysynth. 16 voices (UNISON: stacks of 2..8 voices on one note).
// Voice: VCO-1 (triangle, saw, pulse, square; hard sync to VCO-2; cross-mod: VCO-2 -> VCO-1 exponential FM) +
// VCO-2 (sine, triangle, saw, pulse, noise; coarse + fine tune) -> mixer -> non-resonant 6 dB HPF ->
// IR3109-style low-pass, 12 or 24 dB (feedback from the 4th stage in both, as on the original) -> VCA.
// ENV-1 to VCF and pitch (bipolar, VCO-1 / VCO-2 / both), ENV-2 to VCA, both with key follow (higher notes are
// snappier) and fast, snappy curves for arpeggios. LFO with delay to pitch / PWM / VCF. VOICES: SOLO (mono, legato,
// glide), UNISON (mono stack with detune and spread), POLY 1 (a new note shortens the release tails to 150 ms, the
// tight J-8 POLY 1 feel; tails are never cut off), POLY 2 (tails ring fully).
#include "va1_common.h"

enum { W1, R1, SYNC, XMOD, PW, PWMODE, PLFO, PENV, MODTGT,
       W2, R2, TUNE, FINE,
       LV1, LV2,
       HPF, CUT, RES, SLOPE, ENV, FLFO, KEY,
       A1, D1, S1, RL1, VEL1, KEY1,
       A2, D2, S2, RL2, VEL2, KEY2,
       VOICES, UNIV, UNIDET, LEGATO,
       NS };
#define C(x) VA_C(NS, x)
#define L1WAVE C(CO_L1WAVE)
#define L1RATE C(CO_L1RATE)
#define L1DELAY C(CO_L1DELAY)
#define L2RATE C(CO_L2RATE)
#define L2WAVE C(CO_L2WAVE)
#define BEND C(CO_BEND)
#define GLIDE C(CO_GLIDE)
#define DRIFT C(CO_DRIFT)
#define SPREAD C(CO_SPREAD)
#define MS(n) VA_MOD(NS, n, 0)
#define MD(n) VA_MOD(NS, n, 1)
#define MA(n) VA_MOD(NS, n, 2)

static const char *const PAGES[] = {"OSC 1", "OSC 2", "MIXER", "FILTER", "FILTER ENV", "AMP ENV", "LFO 1", "LFO 2",
                                    "VOICE", "LEVEL", "MOD 1", "MOD 2", "MOD 3", "MOD 4", "MOD 5", "MOD 6", "MOD 7", "MOD 8"};
static const char *const W1S[] = {"TRI", "SAW", "PULSE", "SQUARE"};
static const char *const W2S[] = {"SINE", "TRI", "SAW", "PULSE", "NOISE"};
static const char *const RANGES[] = {"16'", "8'", "4'", "2'"};
static const char *const PWMODES[] = {"MAN", "LFO", "ENV 1"};
static const char *const TGTS[] = {"VCO 1", "VCO 2", "BOTH"};
static const char *const SLOPES[] = {"12 dB", "24 dB"};
static const char *const VOICESS[] = {"SOLO", "UNISON", "POLY 1", "POLY 2"};

static const va_ps PS[NS] = {
    VA_SW("OSC 1", "WAVE", W1S, 1, 1), VA_SW("OSC 1", "RANGE", RANGES, 1, 1), VA_TG("OSC 1", "SYNC", 0, 1),
    VA_KN("OSC 1", "XMOD", F_PCT, 0, 127, 0, 1), VA_KN("OSC 1", "PW", F_PCT, 0, 127, 0, 0),
    VA_SW("OSC 1", "PW MODE", PWMODES, 0, 0), VA_KN("OSC 1", "LFO", F_PCT, 0, 127, 0, 0),
    VA_KN("OSC 1", "ENV 1", F_BIP, -64, 63, 0, 0), VA_SW("OSC 1", "MOD TGT", TGTS, 2, 0),
    VA_SW("OSC 2", "WAVE", W2S, 2, 1), VA_SW("OSC 2", "RANGE", RANGES, 1, 1), VA_KN("OSC 2", "TUNE", F_ST, -12, 12, 0, 1),
    VA_KN("OSC 2", "FINE", F_CENT, -64, 63, 8, 1),
    VA_KN("MIXER", "VCO 1", F_PCT, 0, 127, 100, 1), VA_KN("MIXER", "VCO 2", F_PCT, 0, 127, 100, 1),
    VA_KN("FILTER", "HPF", F_HPF, 0, 127, 0, 1), VA_KN("FILTER", "CUTOFF", F_CUT, 0, 127, 100, 1),
    VA_KN("FILTER", "RESO", F_PCT, 0, 127, 0, 1), VA_SW("FILTER", "SLOPE", SLOPES, 1, 1),
    VA_KN("FILTER", "ENV", F_PCT, 0, 127, 0, 1), VA_KN("FILTER", "LFO", F_PCT, 0, 127, 0, 0),
    VA_KN("FILTER", "KEY", F_PCT, 0, 127, 64, 0),
    VA_KN("FILTER ENV", "ATTACK", F_TATK, 0, 127, 0, 1), VA_KN("FILTER ENV", "DECAY", F_TDEC, 0, 127, 48, 1),
    VA_KN("FILTER ENV", "SUSTAIN", F_PCT, 0, 127, 0, 1), VA_KN("FILTER ENV", "RELEASE", F_TREL, 0, 127, 28, 1),
    VA_KN("FILTER ENV", "VEL", F_PCT, 0, 127, 30, 0), VA_KN("FILTER ENV", "KEY", F_PCT, 0, 127, 20, 0),
    VA_KN("AMP ENV", "ATTACK", F_TATK, 0, 127, 0, 1), VA_KN("AMP ENV", "DECAY", F_TDEC, 0, 127, 48, 1),
    VA_KN("AMP ENV", "SUSTAIN", F_PCT, 0, 127, 127, 1), VA_KN("AMP ENV", "RELEASE", F_TREL, 0, 127, 28, 1),
    VA_KN("AMP ENV", "VEL", F_PCT, 0, 127, 40, 0), VA_KN("AMP ENV", "KEY", F_PCT, 0, 127, 20, 0),
    VA_SW("VOICE", "VOICES", VOICESS, 3, 1), VA_KN("VOICE", "UNI VOICES", F_INT, 2, 8, 4, 0),
    VA_KN("VOICE", "UNI DETUNE", F_PCT, 0, 127, 40, 0), VA_TG("VOICE", "LEGATO", 1, 0),
};

static const char *const DNAME[] = {"OFF", "PITCH", "VCO1 PITCH", "VCO2 PITCH", "PW", "VCO1 LVL", "VCO2 LVL", "XMOD",
                                    "CUTOFF", "RESO", "HPF", "ENV AMT", "ATTACK", "DECAY", "SUSTAIN", "RELEASE",
                                    "LFO1 RATE", "LFO1 AMT", "LFO2 RATE", "LEVEL", "PAN"};
static const unsigned char DID[] = {D_NONE, D_PITCH, D_OSC1, D_OSC2, D_PW, D_LV1, D_LV2, D_XMOD, D_CUT, D_RESO, D_HPF,
                                    D_FENV, D_ATT, D_DEC, D_SUS, D_REL, D_L1RATE, D_L1AMT, D_L2RATE, D_LEVEL, D_PAN};
enum { PD_OFF, PD_PITCH, PD_P1, PD_P2, PD_PW, PD_LV1, PD_LV2, PD_XMOD, PD_CUT, PD_RES, PD_HPF, PD_ENV, PD_ATT, PD_DEC,
       PD_SUS, PD_REL, PD_L1R, PD_L1A, PD_L2R, PD_LEVEL, PD_PAN };

static const va_macro MAC[8] = {
    {"CUTOFF", 1, {{CUT, 0.6f, 0}}},
    {"RESO", 1, {{RES, 1.f, 0}}},
    {"ENV AMT", 3, {{ENV, 1.f, 0}, {CUT, -0.3f, 0}, {PENV, 0.06f, 1}}},   /* more ENV-1: VCF sweep from a darker base + a little pitch "ping" */
    {"ATTACK", 2, {{A1, 1.25f, 0}, {A2, 1.25f, 0}}},
    {"RELEASE", 4, {{RL1, 1.25f, 0}, {RL2, 1.25f, 0}, {D1, 0.4f, 0}, {D2, 0.4f, 0}}},
    {"DETUNE", 4, {{UNIDET, 1.f, 0}, {SPREAD, 0.8f, 0}, {FINE, 0.5f, 0}, {LV2, 0.5f, 1}}},   /* + half: brings the detuned VCO-2 in on 1-osc presets */
    {"XMOD", 2, {{XMOD, 1.f, 0}, {LV1, 0.2f, 0}}},
    {"MOTION", 3, {{L1RATE, 0.2f, 0}, {FLFO, 1.f, 0}, {PLFO, 0.4f, 0}}},
};

static void jp_voicing(va *s, int *mode, int *stack, float *det, int *poly1) {
    switch (s->eff[VOICES]) {
    case 0: *mode = VM_MONO; break;
    case 1: *mode = VM_UNI; *stack = s->eff[UNIV]; *det = PV(s, UNIDET) / 127.f * 0.30f; break;
    case 2: *poly1 = 1; break;
    default: break;
    }
}

enum { K_DT1 = 2, K_DT2, K_W, K_XM, K_LV1, K_LV2, K_HP, K_G, K_K, K_SL, K_AMP, K_IN, K_D };

static void jp_von(va *s, va_voice *v) {
    (void)s;
    v->ph[0] = 0.5f + 0.5f * va_noise(&v->rng);
    v->ph[1] = 0.5f + 0.5f * va_noise(&v->rng);
    v->pend = 0.f; v->o2 = 0.f;
}
static void jp_vctrl(va *s, va_voice *v, int fresh) {
    const float *md = v->md;
    float lamt = 1.f + md[D_L1AMT]; if (lamt < 0.f) lamt = 0.f;
    float lf = v->lval[0] * lamt, e1 = v->env[0].v, dr = PV(s, DRIFT) / 127.f;
    float base = v->pitch + v->detune + s->bend * (float)s->eff[BEND] + md[D_PITCH] * 12.f;
    float x = PV(s, PLFO) / 127.f;
    float pm = lf * x * x * 12.f + e1 * PV(s, PENV) / 64.f * 24.f;
    int tg = s->eff[MODTGT];
    float p1 = base + (float)(s->eff[R1] - 1) * 12.f + (tg != 1 ? pm : 0.f) + md[D_OSC1] * 12.f + dr * v->drift * 0.2f;
    float p2 = base + (float)(s->eff[R2] - 1) * 12.f + PV(s, TUNE) + PV(s, FINE) / 128.f + (tg != 0 ? pm : 0.f)
               + md[D_OSC2] * 12.f + dr * (v->drift * -0.15f + 0.06f * v->dstat);
    float dt1 = va_clampf(va_note_hz(p1) * s->isr, 0.f, 0.45f), dt2 = va_clampf(va_note_hz(p2) * s->isr, 0.f, 0.45f);
    float pw = PV(s, PW) / 127.f, w;
    switch (s->eff[PWMODE]) {
    case 1: w = 0.5f - 0.46f * pw * (0.5f + 0.5f * lf); break;
    case 2: w = 0.5f - 0.46f * pw * e1; break;
    default: w = 0.5f - 0.46f * pw; break;
    }
    w = va_clampf(w + md[D_PW] * 0.5f, 0.04f, 0.96f);
    float xm = PV(s, XMOD) / 127.f;
    xm = va_clampf(xm * xm * 3.f + md[D_XMOD] * 3.f, 0.f, 4.f);
    float vel = v->vel * 1.27f;
    float depth = PV(s, ENV) / 127.f + md[D_FENV];
    depth *= 1.f + PV(s, VEL1) / 127.f * (vel - 1.f);
    float fl = PV(s, FLFO) / 127.f;
    float cut = va_cut_note(PV(s, CUT)) + depth * 84.f * e1 + lf * fl * fl * 40.f + PV(s, KEY) / 127.f * (v->pitch - 60.f)
                + md[D_CUT] * 60.f + dr * v->drift * 1.2f;
    float k = va_clampf((PV(s, RES) / 127.f + md[D_RESO]) * 4.3f, 0.f, 4.5f);
    float hpv = PV(s, HPF), hpa = 0.f;
    if (hpv > 0.f || md[D_HPF] != 0.f) {
        float fc = 8.f * exp2f(va_clampf(hpv / 127.f * 7.5f + md[D_HPF] * 4.f, 0.f, 11.f));
        hpa = 1.f - expf(-2.f * VA_PI * va_clampf(fc, 5.f, 0.4f * s->sr) / s->sr);
    }
    float amp = v->env[1].v * (1.f + PV(s, VEL2) / 127.f * (powf(vel, 1.5f) - 1.f)) * v->ugain
                * va_clampf(1.f + md[D_LEVEL], 0.f, 2.f);
    va_set(v, K_DT1, dt1, fresh); va_set(v, K_DT2, dt2, fresh); va_set(v, K_W, w, fresh); va_set(v, K_XM, xm, fresh);
    va_set(v, K_LV1, va_clampf(PV(s, LV1) / 127.f + md[D_LV1], 0.f, 1.f), fresh);
    va_set(v, K_LV2, va_clampf(PV(s, LV2) / 127.f + md[D_LV2], 0.f, 1.f), fresh);
    va_set(v, K_HP, hpa, fresh);
    float G = va_lad_G(va_note_hz(cut), s->sr);
    va_set(v, K_G, G, fresh);
    va_set(v, K_K, k, fresh);
    va_set(v, K_D, va_lad_D(G, k), fresh);
    va_set(v, K_SL, (float)s->eff[SLOPE], fresh);
    va_set(v, K_AMP, amp * 2.8f, 0);
    va_set(v, K_IN, 0.24f * (1.f + 0.3f * k), fresh);
}

/* waveform value / slope (per phase unit) */
static inline float jp_val(int w, float p, float pw) {
    switch (w) {
    case 0: return p < 0.5f ? 4.f * p - 1.f : 3.f - 4.f * p;
    case 1: return 2.f * p - 1.f;
    case 2: return (p < pw ? 1.f : -1.f) - (2.f * pw - 1.f);
    default: return p < 0.5f ? 1.f : -1.f;
    }
}
static inline float jp_slope(int w, float p) { return w == 0 ? (p < 0.5f ? 4.f : -4.f) : w == 1 ? 2.f : 0.f; }
/* own discontinuities of a VCO (wave 0 tri, 1 saw, 2 pulse, 3 square) between po and p (p already wrapped) */
static inline void jp_events(int w, float po, float p, int wrapped, float dt, float pw, float g, float *aft, float *bef) {
    float edge = w == 2 ? pw : 0.5f;
    if (wrapped) {
        float d = p / dt;
        switch (w) {
        case 0: va_blamp(d, 8.f * dt * g, aft, bef); break;
        case 1: va_blep(d, -2.f * g, aft, bef); break;
        default: va_blep(d, 2.f * g, aft, bef); if (po < edge) va_blep(d, -2.f * g, aft, bef); break;
        }
    } else if (po < edge && p >= edge && w != 1) {
        float d = (p - edge) / dt;
        if (w == 0) va_blamp(d, -8.f * dt * g, aft, bef);
        else va_blep(d, -2.f * g, aft, bef);
    }
}
static void jp_vdsp(va *s, va_voice *v, float *out, int n) {
    float c[VA_NC], ci[VA_NC];                  /* local copies: no aliasing with out[], stays in registers */
    memcpy(c, v->c, sizeof c); memcpy(ci, v->ci, sizeof ci);
    float p1 = v->ph[0], p2 = v->ph[1], pend = v->pend, hp = v->hp;
    int first = v->first;
    int w1 = s->eff[W1], w2 = s->eff[W2], sync = s->eff[SYNC];
    int w2o = w2 == 1 ? 0 : w2 == 2 ? 1 : 2;          /* VCO-2 wave in VCO-1 numbering (tri, saw, pulse) */
    for (int i = 0; i < n; i++) {
        for (int j = K_DT1; j <= K_D; j++) c[j] += ci[j];
        float dt1 = c[K_DT1], dt2 = c[K_DT2], pw = c[K_W], l1 = c[K_LV1], l2 = c[K_LV2];
        float aft = 0.f, bef = 0.f;
        /* VCO-2 */
        float p2o = p2; p2 += dt2;
        int wr2 = p2 >= 1.f; if (wr2) p2 -= 1.f;
        float o2;
        if (w2 == 0) o2 = va_sin(p2);
        else if (w2 == 4) o2 = va_noise(&v->rng) * s->nzk;
        else { o2 = jp_val(w2o, p2, pw); jp_events(w2o, p2o, p2, wr2, dt2, pw, l2, &aft, &bef); }
        /* VCO-1 (cross-mod from VCO-2) */
        float dtf = dt1;
        if (c[K_XM] > 1e-4f) dtf = va_clampf(dt1 * va_exp2(c[K_XM] * o2), 0.f, 0.45f);
        float p1o = p1; p1 += dtf;
        int wr1 = p1 >= 1.f; if (wr1) p1 -= 1.f;
        jp_events(w1, p1o, p1, wr1, dtf, pw, l1, &aft, &bef);
        if (sync && wr2) {                                  /* hard sync: VCO-1 restarts when VCO-2 wraps */
            float d2 = p2 / dt2;
            float pe = p1 - d2 * dtf; if (pe < 0.f) pe += 1.f;
            va_blep(d2, (jp_val(w1, 0.f, pw) - jp_val(w1, pe, pw)) * l1, &aft, &bef);
            if (w1 == 0) va_blamp(d2, (jp_slope(0, 0.f) - jp_slope(0, pe)) * dtf * l1, &aft, &bef);
            p1 = d2 * dtf;
        }
        float x = l1 * jp_val(w1, p1, pw) + l2 * o2;
        float o = pend + bef;
        if (first) { o = x; first = 0; }
        pend = x + aft;
        /* HPF (one pole) */
        hp += c[K_HP] * (o - hp);
        o -= hp;
        float y2, y4 = va_lad_tick(&v->lad, o * c[K_IN], c[K_G], c[K_K], c[K_D], &y2);
        float sl = c[K_SL];
        out[i] = (y4 * sl + y2 * 0.8f * (1.f - sl)) * c[K_AMP];
    }
    memcpy(v->c + 2, c + 2, sizeof(float) * (VA_NC - 2));
    v->first = first; v->ph[0] = p1; v->ph[1] = p2; v->pend = pend; v->hp = fabsf(hp) < 1e-20f ? 0.f : hp;
    if (fabsf(v->lad.s1) < 1e-20f) v->lad.s1 = 0.f;
    if (fabsf(v->lad.s2) < 1e-20f) v->lad.s2 = 0.f;
    if (fabsf(v->lad.s3) < 1e-20f) v->lad.s3 = 0.f;
    if (fabsf(v->lad.s4) < 1e-20f) v->lad.s4 = 0.f;
}

/* ------------------------------------------------------------------ presets */
#define P(...) {__VA_ARGS__, -1}
#define ENV1(a, d, s_, r) A1, a, D1, d, S1, s_, RL1, r
#define ENV2(a, d, s_, r) A2, a, D2, d, S2, s_, RL2, r
static const char ARP1[] = "{\"mode\":0,\"rate\":4,\"oct\":2,\"gate\":40}";
static const char ARP2[] = "{\"mode\":2,\"rate\":4,\"oct\":1,\"gate\":35}";
static const char ARP3[] = "{\"mode\":8,\"rate\":4,\"oct\":1,\"gate\":40}";
static const char ARP4[] = "{\"mode\":1,\"rate\":5,\"oct\":2,\"gate\":45}";

static const va_preset PRE[] = {
    {"INIT", "Init", 0, NULL, P(LV2, 0)},
    /* ---- Bass */
    {"Titan Bass", "Bass", 0, NULL, P(R1, 0, R2, 0, W1, 1, W2, 3, FINE, 10, CUT, 40, RES, 30, ENV, 62, ENV1(0, 39, 20, 18), ENV2(0, 48, 90, 16), VOICES, 0, LEGATO, 0)},
    {"Psy Roller", "Bass", 0, NULL, P(R1, 0, R2, 0, W1, 1, W2, 2, FINE, 6, LV2, 70, CUT, 32, RES, 46, ENV, 72, ENV1(0, 31, 0, 14), ENV2(0, 36, 40, 13), VEL1, 60, VOICES, 3, KEY, 70)},
    {"Sync Bass", "Bass", 0, NULL, P(R1, 1, R2, 0, SYNC, 1, W1, 1, W2, 2, LV2, 40, PENV, 30, CUT, 52, RES, 20, ENV, 50, ENV1(0, 40, 10, 18), ENV2(0, 52, 80, 18), VOICES, 0)},
    {"Unison Growl", "Bass", 0, NULL, P(R1, 0, R2, 0, W1, 1, W2, 2, VOICES, 1, UNIV, 4, UNIDET, 50, CUT, 40, RES, 26, ENV, 56, ENV1(0, 46, 20, 18), ENV2(0, 59, 100, 18), SPREAD, 60)},
    {"Tech Pulse", "Bass", 0, NULL, P(R1, 0, R2, 0, W1, 2, PW, 50, W2, 0, TUNE, -12, LV2, 80, CUT, 36, RES, 56, ENV, 66, ENV1(0, 34, 0, 14), ENV2(0, 39, 30, 14), VOICES, 0, LEGATO, 0)},
    /* ---- Lead */
    {"Sync Lead", "Lead", 0, NULL, P(SYNC, 1, W1, 1, W2, 2, LV2, 0, PENV, 40, ENV1(0, 54, 30, 28), ENV2(0, 48, 110, 28), CUT, 90, RES, 20, ENV, 30, PLFO, 18, L1RATE, 96, L1DELAY, 50, VOICES, 0, GLIDE, 24)},
    {"Brass Solo", "Lead", 0, NULL, P(W1, 1, W2, 2, FINE, 14, CUT, 58, RES, 18, ENV, 60, ENV1(21, 48, 60, 28), ENV2(10, 48, 110, 28), VOICES, 0, GLIDE, 30, PLFO, 12, L1RATE, 94, L1DELAY, 64)},
    {"Unison Scream", "Lead", 0, NULL, P(VOICES, 1, UNIV, 6, UNIDET, 60, W1, 1, W2, 2, CUT, 76, RES, 40, ENV, 40, ENV1(0, 48, 60, 28), ENV2(0, 48, 120, 31), SPREAD, 90, GLIDE, 20)},
    {"XMod Lead", "Lead", 0, NULL, P(W1, 1, W2, 0, R2, 0, XMOD, 54, LV2, 0, CUT, 84, RES, 30, ENV, 30, ENV1(0, 52, 40, 28), ENV2(0, 48, 110, 28), VOICES, 0, MS(2), S_WHEEL, MD(2), PD_XMOD, MA(2), 40)},
    /* ---- Pad */
    {"Titan Pad", "Pad", 0, NULL, P(W1, 1, W2, 2, FINE, 14, CUT, 70, RES, 10, ENV, 24, ENV1(59, 67, 60, 60), ENV2(56, 67, 110, 63), PLFO, 4, L1RATE, 70, VOICES, 3, SPREAD, 60, VEL2, 20)},
    {"Warm Haze", "Pad", 0, NULL, P(W1, 2, PW, 40, PWMODE, 1, W2, 2, FINE, 10, L1RATE, 66, CUT, 64, RES, 20, ENV, 30, ENV1(67, 72, 50, 65), ENV2(64, 72, 116, 68), VOICES, 3, SPREAD, 80, SLOPE, 0)},
    {"Glass Cathedral", "Pad", 0, NULL, P(W1, 0, W2, 0, R2, 2, LV1, 110, LV2, 70, HPF, 50, CUT, 90, RES, 30, ENV, 10, ENV1(72, 79, 80, 71), ENV2(72, 79, 120, 74), VOICES, 3, SPREAD, 100, MS(2), S_LFO2, MD(2), PD_CUT, MA(2), 8, L2RATE, 40)},
    {"Psy Dusk", "Pad", 0, NULL, P(W1, 1, W2, 1, R2, 2, FINE, 20, CUT, 60, RES, 40, ENV, 36, ENV1(72, 82, 40, 71), ENV2(67, 72, 116, 71), MS(2), S_LFO2, MD(2), PD_CUT, MA(2), 16, L2RATE, 46, L2WAVE, 1, VOICES, 3, SPREAD, 90)},
    {"Unison Choir", "Pad", 0, NULL, P(VOICES, 1, UNIV, 8, UNIDET, 64, W1, 2, PW, 60, PWMODE, 1, W2, 0, LV2, 50, L1RATE, 60, CUT, 66, RES, 30, ENV, 16, ENV1(72, 72, 80, 65), ENV2(69, 72, 120, 65), SPREAD, 127, LEGATO, 1, GLIDE, 16)},
    /* ---- Strings */
    {"Poly Strings", "Strings", 0, NULL, P(W1, 1, W2, 1, FINE, 12, HPF, 40, CUT, 84, ENV, 10, ENV1(48, 59, 80, 53), ENV2(46, 59, 120, 53), PLFO, 6, L1RATE, 92, L1DELAY, 60, VOICES, 3, SPREAD, 70, VEL2, 30)},
    {"Cinema Strings", "Strings", 0, NULL, P(W1, 1, W2, 2, FINE, 18, HPF, 30, CUT, 66, RES, 14, ENV, 40, ENV1(67, 79, 60, 65), ENV2(67, 72, 120, 65), VOICES, 3, SPREAD, 90, MS(2), S_WHEEL, MD(2), PD_CUT, MA(2), 30)},
    /* ---- Brass */
    {"Titan Brass", "Brass", 0, NULL, P(W1, 1, W2, 1, FINE, 12, CUT, 52, RES, 14, ENV, 70, ENV1(24, 48, 60, 28), ENV2(16, 48, 110, 28), VEL1, 50, VOICES, 3, SPREAD, 50)},
    {"Synthwave Horns", "Brass", 0, NULL, P(W1, 1, W2, 3, PW, 30, FINE, 16, CUT, 56, RES, 20, ENV, 76, ENV1(21, 44, 70, 27), ENV2(13, 44, 116, 27), VEL1, 60, PLFO, 6, L1RATE, 90, L1DELAY, 70, VOICES, 3, SPREAD, 60)},
    {"Stab 1984", "Brass", 0, NULL, P(W1, 1, W2, 1, FINE, 20, CUT, 50, RES, 22, ENV, 84, ENV1(0, 42, 30, 28), ENV2(0, 46, 70, 28), VEL1, 70, VOICES, 2)},
    /* ---- Keys */
    {"Poly Keys", "Keys", 0, NULL, P(W1, 2, PW, 30, W2, 0, R2, 2, LV2, 50, CUT, 56, RES, 10, ENV, 54, ENV1(0, 61, 10, 40), ENV2(0, 67, 0, 41), VEL1, 60, VEL2, 70, VOICES, 3)},
    {"Funk Clav", "Keys", 0, NULL, P(W1, 2, PW, 90, W2, 3, R2, 2, LV2, 60, HPF, 60, CUT, 48, RES, 50, ENV, 70, ENV1(0, 39, 0, 23), ENV2(0, 46, 0, 23), VEL1, 70, VEL2, 60, VOICES, 2)},
    {"Bright EP", "Keys", 0, NULL, P(W1, 0, W2, 0, R2, 3, LV2, 40, XMOD, 30, CUT, 70, ENV, 40, ENV1(0, 59, 0, 43), ENV2(0, 69, 0, 45), VEL1, 50, VEL2, 70, VOICES, 3, MS(2), S_LFO2, MD(2), PD_LEVEL, MA(2), 10, L2RATE, 80)},
    /* ---- Pluck */
    {"Titan Pluck", "Pluck", 0, NULL, P(W1, 1, W2, 2, FINE, 10, CUT, 44, RES, 40, ENV, 74, ENV1(0, 37, 0, 28), ENV2(0, 46, 0, 32), VOICES, 3, SPREAD, 60)},
    {"Sync Pluck", "Pluck", 0, NULL, P(SYNC, 1, W1, 1, W2, 2, LV2, 0, R1, 2, PENV, 36, CUT, 70, RES, 30, ENV, 50, ENV1(0, 39, 0, 28), ENV2(0, 46, 0, 32), VOICES, 3)},
    {"Harp Drop", "Pluck", 0, NULL, P(W1, 0, W2, 2, R2, 2, LV2, 60, CUT, 54, RES, 50, KEY, 110, ENV, 60, ENV1(0, 44, 0, 43), ENV2(0, 59, 0, 53), VOICES, 3, SPREAD, 80)},
    /* ---- Arp (short psy plucks) */
    {"Titan Arp", "Arp", JF_ARP, ARP1, P(W1, 1, W2, 2, FINE, 8, CUT, 42, RES, 52, ENV, 80, ENV1(0, 31, 0, 18), ENV2(0, 36, 0, 18), VOICES, 2, KEY1, 60, KEY2, 60)},
    {"Psy Sparks", "Arp", JF_ARP, ARP2, P(W1, 2, PW, 60, PWMODE, 1, L1RATE, 98, W2, 2, R2, 2, LV2, 50, CUT, 46, RES, 62, ENV, 76, ENV1(0, 30, 0, 17), ENV2(0, 33, 0, 17), VOICES, 2)},
    {"Sync Arp", "Arp", JF_ARP, ARP3, P(SYNC, 1, W1, 1, W2, 2, LV2, 0, PENV, 44, CUT, 60, RES, 36, ENV, 60, ENV1(0, 31, 0, 18), ENV2(0, 34, 0, 18), VOICES, 2)},
    {"Pulse Arp 85", "Arp", JF_ARP, ARP4, P(W1, 2, PW, 40, W2, 3, FINE, 12, CUT, 50, RES, 40, ENV, 70, ENV1(0, 33, 0, 20), ENV2(0, 37, 10, 20), VOICES, 2, SPREAD, 70)},
    /* ---- Bell */
    {"Cross Bell", "Bell", 0, NULL, P(W1, 0, W2, 0, R2, 3, XMOD, 70, LV2, 0, CUT, 96, RES, 10, ENV, 20, ENV1(0, 72, 0, 65), ENV2(0, 79, 0, 71), VEL2, 50, VOICES, 3, SPREAD, 70)},
    {"Tubular", "Bell", 0, NULL, P(W1, 3, W2, 0, R2, 2, TUNE, 7, XMOD, 40, LV2, 60, HPF, 50, CUT, 84, RES, 30, ENV, 30, ENV1(0, 67, 0, 60), ENV2(0, 75, 0, 68), VOICES, 3)},
    /* ---- Drone */
    {"Titan Drone", "Drone", JF_HOLD, NULL, P(R1, 0, R2, 0, W1, 1, W2, 2, FINE, 6, CUT, 46, RES, 50, FLFO, 30, L1RATE, 30, ENV1(89, 89, 127, 80), ENV2(89, 89, 127, 80), VOICES, 3, SPREAD, 80, VEL1, 0, VEL2, 0)},
    {"Sync Drone", "Drone", 0, NULL, P(R1, 0, SYNC, 1, W1, 1, W2, 2, LV2, 30, MS(2), S_LFO2, MD(2), PD_P1, MA(2), 14, L2RATE, 30, CUT, 60, RES, 30, ENV1(79, 89, 127, 77), ENV2(79, 89, 127, 77), VOICES, 3, VEL2, 0)},
    /* ---- Texture */
    {"Noise Ocean", "Texture", 0, NULL, P(W1, 0, LV1, 30, W2, 4, LV2, 127, CUT, 64, RES, 70, KEY, 127, FLFO, 60, L1RATE, 36, ENV1(72, 72, 127, 71), ENV2(72, 72, 127, 77), VOICES, 3, SPREAD, 100)},
    {"Metal Rain", "Texture", 0, NULL, P(W1, 3, XMOD, 90, W2, 1, R2, 3, LV2, 40, HPF, 70, CUT, 90, RES, 50, ENV1(31, 72, 60, 65), ENV2(48, 72, 100, 71), MS(2), S_LFO2, MD(2), PD_XMOD, MA(2), 20, L2RATE, 50, VOICES, 3, SPREAD, 100)},
    /* ---- FX */
    {"Laser Sync", "FX", 0, NULL, P(SYNC, 1, W1, 1, W2, 2, LV2, 0, PENV, 63, ENV1(0, 52, 0, 35), CUT, 100, RES, 30, ENV2(0, 59, 60, 35), VOICES, 0)},
    {"Siren Rise", "FX", 0, NULL, P(W1, 1, W2, 2, PLFO, 60, L1WAVE, 2, L1RATE, 60, CUT, 80, RES, 50, FLFO, 30, ENV2(31, 72, 127, 53), VOICES, 0)},
    /* ---- Sequence */
    {"Berlin Seq", "Sequence", 0, NULL, P(W1, 1, W2, 2, R2, 0, FINE, 10, CUT, 42, RES, 56, ENV, 64, ENV1(0, 31, 0, 15), ENV2(0, 36, 0, 15), VEL1, 60, VOICES, 2)},
    {"Arp Seq 8", "Sequence", 0, NULL, P(W1, 2, PW, 50, W2, 0, R2, 0, CUT, 48, RES, 44, ENV, 66, ENV1(0, 33, 0, 16), ENV2(0, 39, 10, 16), VOICES, 0, LEGATO, 0)},
};
#define NPRE ((int)(sizeof PRE / sizeof PRE[0]))

#if __has_include("va_trims.h")
#include "va_trims.h"
#define JP_TRIMS TRIM_TITAN
#else
#define JP_TRIMS NULL
#endif

const va_eng VA_ENG_TITAN = {
    .id = "titan", .npages = (int)(sizeof PAGES / sizeof PAGES[0]), .pages = PAGES, .ns = NS, .ps = PS,
    .ndest = (int)(sizeof DNAME / sizeof DNAME[0]), .dname = DNAME, .did = DID, .npre = NPRE, .pre = PRE, .mac = MAC,
    .trim = JP_TRIMS, .poly = 16,
    .envp = {{A1, D1, S1, RL1}, {A2, D2, S2, RL2}}, .envkey = {KEY1, KEY2}, .p_vcagate = -1, .p_cutoff = CUT,
    .p_envtrig = -1, .p_porta = -1, .p_legato = LEGATO,
    .voicing = jp_voicing, .von = jp_von, .vctrl = jp_vctrl, .vdsp = jp_vdsp, .post = NULL, .post_reset = NULL,
};
static void *jp_create(double sr) { return va_create(&VA_ENG_TITAN, sr); }
const jeng_def_t JE_TITAN = {.id = "titan", .name = "TITAN", .family = "Analog", .poly = 16, VA_JENG_FIELDS(jp_create)};
