// SPDX-License-Identifier: GPL-3.0-only
// Jammer classic1: AURA, a classic chorus polysynth. 16 voices.
// Voice: one DCO (saw, pulse with PWM from LFO / manual / envelope, sub square one octave down, white noise) ->
// IR3109-style 24 dB resonant low-pass (self-oscillates) -> VCA (ENV or GATE). One ADSR feeds VCF and VCA.
// One LFO with delay (+ LFO 2 for the matrix). After the voices: the non-resonant HPF (0 = bass boost, 1 = flat,
// 2 = 240 Hz, 3 = 720 Hz), then the BBD stereo chorus I / II / I+II (two modulated delay lines, triangle LFO,
// left and right in anti-phase, pre/post anti-alias low-pass for the dark BBD tone, compander-style hiss that
// follows the signal). The DCO is stable (DRIFT default low): the Aura sound is its chorus and its filter.
#include "va1_common.h"

enum { RANGE, SAW, PULSE, PWMODE, PW, DLFO, FINE,
       SUB, NOISE,
       HPF, CUT, RES, ENV, ENVPOL, VLFO, KEY, FVEL,
       ATT, DEC, SUS, REL, AVEL, VCA,
       CHO, CDEPTH, HISS,
       NS };
#define C(x) VA_C(NS, x)
#define L1WAVE C(CO_L1WAVE)
#define L1RATE C(CO_L1RATE)
#define L1SYNC C(CO_L1SYNC)
#define L1DELAY C(CO_L1DELAY)
#define L1RETRIG C(CO_L1RETRIG)
#define L2WAVE C(CO_L2WAVE)
#define L2RATE C(CO_L2RATE)
#define BEND C(CO_BEND)
#define GLIDE C(CO_GLIDE)
#define DRIFT C(CO_DRIFT)
#define LEVEL C(CO_LEVEL)
#define PAN C(CO_PAN)
#define SPREAD C(CO_SPREAD)
#define MS(n) VA_MOD(NS, n, 0)
#define MD(n) VA_MOD(NS, n, 1)
#define MA(n) VA_MOD(NS, n, 2)

static const char *const PAGES[] = {"OSC 1", "MIXER", "FILTER", "AMP ENV", "LFO 1", "LFO 2", "FX", "VOICE", "LEVEL",
                                    "MOD 1", "MOD 2", "MOD 3", "MOD 4", "MOD 5", "MOD 6", "MOD 7", "MOD 8"};
static const char *const RANGES[] = {"16'", "8'", "4'"};
static const char *const PWMODES[] = {"MAN", "LFO", "ENV"};
static const char *const HPFS[] = {"0 BOOST", "1 FLAT", "2 240 Hz", "3 720 Hz"};
static const char *const POLS[] = {"+", "-"};
static const char *const VCAS[] = {"ENV", "GATE"};
static const char *const CHOS[] = {"OFF", "I", "II", "I+II"};

static const va_ps PS[NS] = {
    VA_SW("OSC 1", "RANGE", RANGES, 1, 1), VA_TG("OSC 1", "SAW", 1, 1), VA_TG("OSC 1", "PULSE", 0, 1),
    VA_SW("OSC 1", "PW MODE", PWMODES, 0, 1), VA_KN("OSC 1", "PW", F_PCT, 0, 127, 0, 1),
    VA_KN("OSC 1", "LFO", F_PCT, 0, 127, 0, 0), VA_KN("OSC 1", "FINE", F_CENT, -64, 63, 0, 0),
    VA_KN("MIXER", "SUB", F_PCT, 0, 127, 0, 1), VA_KN("MIXER", "NOISE", F_PCT, 0, 127, 0, 1),
    VA_SW("FILTER", "HPF", HPFS, 1, 1), VA_KN("FILTER", "CUTOFF", F_CUT, 0, 127, 100, 1),
    VA_KN("FILTER", "RESO", F_PCT, 0, 127, 0, 1), VA_KN("FILTER", "ENV", F_PCT, 0, 127, 0, 1),
    VA_SW("FILTER", "ENV POL", POLS, 0, 0), VA_KN("FILTER", "LFO", F_PCT, 0, 127, 0, 1),
    VA_KN("FILTER", "KEY", F_PCT, 0, 127, 64, 0), VA_KN("FILTER", "VEL", F_PCT, 0, 127, 30, 0),
    VA_KN("AMP ENV", "ATTACK", F_TATK, 0, 127, 0, 1), VA_KN("AMP ENV", "DECAY", F_TDEC, 0, 127, 48, 1),
    VA_KN("AMP ENV", "SUSTAIN", F_PCT, 0, 127, 127, 1), VA_KN("AMP ENV", "RELEASE", F_TREL, 0, 127, 28, 1),
    VA_KN("AMP ENV", "VEL", F_PCT, 0, 127, 40, 0), VA_SW("AMP ENV", "VCA", VCAS, 0, 0),
    VA_SW("FX", "CHORUS", CHOS, 0, 1), VA_KN("FX", "DEPTH", F_PCT, 0, 127, 64, 0), VA_KN("FX", "HISS", F_PCT, 0, 127, 40, 0),
};

static const char *const DNAME[] = {"OFF", "PITCH", "PW", "SUB LVL", "NOISE LVL", "CUTOFF", "RESO", "ENV AMT",
                                    "ATTACK", "DECAY", "SUSTAIN", "RELEASE", "LFO1 RATE", "LFO1 AMT", "LFO2 RATE",
                                    "LEVEL", "PAN", "CHORUS"};
static const unsigned char DID[] = {D_NONE, D_PITCH, D_PW, D_LV3, D_LVN, D_CUT, D_RESO, D_FENV, D_ATT, D_DEC, D_SUS,
                                    D_REL, D_L1RATE, D_L1AMT, D_L2RATE, D_LEVEL, D_PAN, D_FX};
enum { JD_OFF, JD_PITCH, JD_PW, JD_SUB, JD_NOISE, JD_CUT, JD_RES, JD_ENV, JD_ATT, JD_DEC, JD_SUS, JD_REL, JD_L1R,
       JD_L1A, JD_L2R, JD_LEVEL, JD_PAN, JD_CHO };

static const va_macro MAC[8] = {
    {"CUTOFF", 1, {{CUT, 0.5f, 0}}},
    {"RESO", 1, {{RES, 1.f, 0}}},
    {"ENV AMT", 1, {{ENV, 1.f, 0}}},
    {"ATTACK", 1, {{ATT, 1.25f, 0}}},
    {"RELEASE", 2, {{REL, 1.25f, 0}, {DEC, 0.5f, 0}}},
    {"PW + SUB", 2, {{PW, 1.5f, 0}, {SUB, 1.5f, 0}}},
    {"MOTION", 3, {{L1RATE, 0.4f, 0}, {VLFO, 2.f, 0}, {DLFO, 0.5f, 0}}},
    {"CHORUS", 3, {{CDEPTH, 1.f, 0}, {CHO, 1.f, 0}, {HISS, 0.3f, 0}}},
};

/* control channels */
enum { K_DT = 2, K_W, K_SAW, K_PUL, K_SUB, K_NOI, K_G, K_K, K_AMP, K_IN, K_D };

static void ju_von(va *s, va_voice *v) {
    (void)s;
    v->ph[0] = 0.5f + 0.5f * va_noise(&v->rng);   /* free-running DCO: any phase */
    v->flip = 0; v->pend = 0.f;
}
static void ju_vctrl(va *s, va_voice *v, int fresh) {
    const float *md = v->md;
    float lamt = 1.f + md[D_L1AMT]; if (lamt < 0.f) lamt = 0.f;
    float lf = v->lval[0] * lamt, env = v->env[0].v, dr = PV(s, DRIFT) / 127.f * v->drift;
    /* pitch */
    float x = PV(s, DLFO) / 127.f;
    float p = v->pitch + v->detune + (float)(s->eff[RANGE] - 1) * 12.f + PV(s, FINE) / 128.f + s->bend * (float)s->eff[BEND]
              + lf * x * x * 12.f + md[D_PITCH] * 12.f + dr * 0.12f;
    float dt = va_note_hz(p) * s->isr; if (dt > 0.45f) dt = 0.45f;
    /* pulse width */
    float pw = PV(s, PW) / 127.f, w;
    switch (s->eff[PWMODE]) {
    case 1: w = 0.5f - 0.46f * pw * (0.5f + 0.5f * lf); break;
    case 2: w = 0.5f - 0.46f * pw * env; break;
    default: w = 0.5f - 0.46f * pw; break;
    }
    w = va_clampf(w + md[D_PW] * 0.5f, 0.04f, 0.96f);
    /* filter */
    float vel = v->vel * 1.27f;
    float depth = PV(s, ENV) / 127.f + md[D_FENV];
    depth *= 1.f + PV(s, FVEL) / 127.f * (vel - 1.f);
    float fv = PV(s, VLFO) / 127.f;
    float cut = va_cut_note(PV(s, CUT)) + depth * 84.f * (s->eff[ENVPOL] ? -env : env) + lf * fv * fv * 40.f
                + PV(s, KEY) / 127.f * (v->pitch - 60.f) + md[D_CUT] * 60.f + dr * 1.5f;
    float k = va_clampf((PV(s, RES) / 127.f + md[D_RESO]) * 4.25f, 0.f, 4.4f);
    /* amp */
    float av = PV(s, AVEL) / 127.f;
    float amp = v->env[1].v * (1.f + av * (powf(vel, 1.5f) - 1.f)) * v->ugain * va_clampf(1.f + md[D_LEVEL], 0.f, 2.f);
    va_set(v, K_DT, dt, fresh);
    va_set(v, K_W, w, fresh);
    va_set(v, K_SAW, (float)s->eff[SAW], fresh);
    va_set(v, K_PUL, (float)s->eff[PULSE], fresh);
    va_set(v, K_SUB, va_clampf(PV(s, SUB) / 127.f + md[D_LV3], 0.f, 1.f), fresh);
    float nz = va_clampf(PV(s, NOISE) / 127.f + md[D_LVN], 0.f, 1.f);
    va_set(v, K_NOI, nz * nz * 1.2f * s->nzk, fresh);
    float G = va_lad_G(va_note_hz(cut), s->sr);
    va_set(v, K_G, G, fresh);
    va_set(v, K_K, k, fresh);
    va_set(v, K_D, va_lad_D(G, k), fresh);
    va_set(v, K_AMP, amp * 1.3f, 0);
    va_set(v, K_IN, 0.30f * (1.f + 0.3f * k), fresh);
}
static void ju_vdsp(va *s, va_voice *v, float *out, int n) {
    (void)s;
    float c[VA_NC], ci[VA_NC];                  /* local copies: no aliasing with out[], stays in registers */
    memcpy(c, v->c, sizeof c); memcpy(ci, v->ci, sizeof ci);
    float p = v->ph[0], pend = v->pend;
    int first = v->first;
    int flip = v->flip;
    for (int i = 0; i < n; i++) {
        for (int j = K_DT; j <= K_D; j++) c[j] += ci[j];
        float dt = c[K_DT], w = c[K_W], ls = c[K_SAW], lp = c[K_PUL], lb = c[K_SUB];
        float po = p; p += dt;
        float aft = 0.f, bef = 0.f;
        if (p >= 1.f) {
            p -= 1.f;
            float d = p / dt;
            flip ^= 1;
            va_blep(d, -2.f * ls, &aft, &bef);                 /* saw wrap */
            if (po < w) va_blep(d, -2.f * lp, &aft, &bef);   /* (pulse fell inside this sample too: rare) */
            va_blep(d, 2.f * lp, &aft, &bef);                  /* pulse rises */
            va_blep(d, (flip ? -2.f : 2.f) * lb, &aft, &bef);  /* sub flips */
        } else if (po < w && p >= w) {
            va_blep((p - w) / dt, -2.f * lp, &aft, &bef);
        }
        float x = ls * (2.f * p - 1.f) + lp * ((p < w ? 1.f : -1.f) - (2.f * w - 1.f)) + lb * (flip ? -1.f : 1.f)
                  + c[K_NOI] * va_noise(&v->rng);
        float o = pend + bef;
        if (first) { o = x; first = 0; }
        pend = x + aft;
        float y2, y = va_lad_tick(&v->lad, o * c[K_IN], c[K_G], c[K_K], c[K_D], &y2);
        out[i] = y * c[K_AMP];
    }
    memcpy(v->c + 2, c + 2, sizeof(float) * (VA_NC - 2));
    v->first = first; v->ph[0] = p; v->pend = pend; v->flip = flip;
    if (fabsf(v->lad.s1) < 1e-20f) v->lad.s1 = 0.f;
    if (fabsf(v->lad.s2) < 1e-20f) v->lad.s2 = 0.f;
    if (fabsf(v->lad.s3) < 1e-20f) v->lad.s3 = 0.f;
    if (fabsf(v->lad.s4) < 1e-20f) v->lad.s4 = 0.f;
}

/* ------------------------------------------------------------------ HPF + BBD chorus */
static const float CH_RATE[4] = {0.f, 0.513f, 0.863f, 9.75f};
static const float CH_CEN[4] = {3.5f, 3.5f, 3.5f, 3.5f};
static const float CH_RNG[4] = {0.f, 1.85f, 1.85f, 0.25f};
static void ju_targets(va *s, float *g, float *hx, float *hh, float *hl, float *cen, float *rng, float *wet) {
    static const float FC[4] = {80.f, 240.f, 240.f, 720.f};
    int m = s->eff[HPF];
    float fc = FC[m];
    *g = tanf(VA_PI * fc / s->sr);
    *hx = m <= 1 ? 1.f : 0.f; *hh = m >= 2 ? 1.f : 0.f; *hl = m == 0 ? 1.f : 0.f;
    int c = s->eff[CHO];
    float depth = va_clampf((float)s->eff[CDEPTH] / 64.f + va_gmod(s, D_FX) * 2.f, 0.f, 2.f);
    *cen = CH_CEN[c]; *rng = CH_RNG[c] * depth; *wet = c ? 1.f : 0.f;
    if (*cen - *rng < 0.4f) *rng = *cen - 0.4f;
}
static void ju_post_reset(va *s) {
    memset(&s->ju, 0, sizeof s->ju);
    s->ju.nrng = 0x2545F491u;
    ju_targets(s, &s->ju.hg, &s->ju.hx, &s->ju.hh, &s->ju.hl, &s->ju.cen, &s->ju.rng_ms, &s->ju.wet);
}
static void ju_post(va *s, float *L, float *R, int n) {
    float g, hxt, hht, hlt, cent, rngt, wett;
    ju_targets(s, &g, &hxt, &hht, &hlt, &cent, &rngt, &wett);
    float rate = CH_RATE[s->eff[CHO]] * s->isr;
    float sk = 1.f - expf(-1.f / (0.02f * s->sr));
    float hk = 1.4142f;                                   /* SVF damping (Q 0.707) */
    float msr = 0.001f * s->sr;
    float apre = 1.f - expf(-2.f * VA_PI * 9000.f / s->sr), apost = 1.f - expf(-2.f * VA_PI * 6500.f / s->sr);
    float ahl = 1.f - expf(-2.f * VA_PI * 7000.f / s->sr), ahh = 1.f - expf(-2.f * VA_PI * 900.f / s->sr);
    float erel = expf(-1.f / (0.15f * s->sr));
    float hiss = (float)s->eff[HISS] / 127.f * 0.005f;
    __typeof__(s->ju) *J = &s->ju;
    float *ch[2] = {L, R};
    /* nothing in, the delay tails gone (> 30 ms) and the hiss below -110 dB: skip (keeps the LFO phase and the write position moving) */
    int silent = 1;
    for (int i = 0; i < n; i++) if (L[i] != 0.f || R[i] != 0.f) { silent = 0; break; }
    J->idle = silent ? J->idle + n : 0;
    if (silent && J->idle > (int)(0.03f * s->sr) && J->env < 1e-4f) {
        float ph = J->ph + rate * (float)n; J->ph = ph - floorf(ph);
        J->w = (J->w + n) & (VA_CHBUF - 1);
        J->hg = g; J->hx = hxt; J->hh = hht; J->hl = hlt; J->cen = cent; J->rng_ms = rngt; J->wet = wett;
        memset(J->hs, 0, sizeof J->hs); memset(J->pre, 0, sizeof J->pre); J->post[0] = J->post[1] = 0.f; J->env = 0.f;
        return;
    }
    for (int i = 0; i < n; i++) {
        J->hg += (g - J->hg) * sk; J->hx += (hxt - J->hx) * sk; J->hh += (hht - J->hh) * sk; J->hl += (hlt - J->hl) * sk;
        J->cen += (cent - J->cen) * sk; J->rng_ms += (rngt - J->rng_ms) * sk; J->wet += (wett - J->wet) * sk;
        float a1 = 1.f / (1.f + J->hg * (J->hg + hk)), a2 = J->hg * a1, a3 = J->hg * a2;
        J->ph += rate; if (J->ph >= 1.f) J->ph -= 1.f;
        float tri = J->ph < 0.5f ? 4.f * J->ph - 1.f : 3.f - 4.f * J->ph;
        /* hiss: band-limited noise, level follows the signal (compander) */
        float wn = va_noise(&J->nrng);
        J->hissz[0] += ahl * (wn - J->hissz[0]); J->hissz[1] += ahh * (J->hissz[0] - J->hissz[1]);
        float hz = (J->hissz[0] - J->hissz[1]) * hiss * va_clampf(J->env * 4.f, 0.f, 1.f);
        float mx = 0.f;
        for (int c2 = 0; c2 < 2; c2++) {
            float x = ch[c2][i];
            /* HPF */
            float *ic = J->hs[c2];
            float v3 = x - ic[1], v1 = a1 * ic[0] + a2 * v3, v2 = ic[1] + a2 * ic[0] + a3 * v3;
            ic[0] = 2.f * v1 - ic[0]; ic[1] = 2.f * v2 - ic[1];
            float hp = x - hk * v1 - v2;
            x = x * J->hx + hp * J->hh + v2 * J->hl;
            float ax = fabsf(x); if (ax > mx) mx = ax;
            /* BBD */
            J->pre[c2][0] += apre * (x - J->pre[c2][0]); J->pre[c2][1] += apre * (J->pre[c2][0] - J->pre[c2][1]);
            J->buf[c2][J->w] = J->pre[c2][1];
            float d = (J->cen + (c2 ? -J->rng_ms : J->rng_ms) * tri) * msr;
            float rp = (float)J->w - d; if (rp < 0.f) rp += (float)VA_CHBUF;
            int i0 = (int)rp; float fr = rp - (float)i0;
            float y = J->buf[c2][i0 & (VA_CHBUF - 1)] * (1.f - fr) + J->buf[c2][(i0 + 1) & (VA_CHBUF - 1)] * fr;
            J->post[c2] += apost * (y - J->post[c2]);
            float wv = va_tanh(J->post[c2] * 1.2f) * (1.f / 1.2f) + (c2 ? -hz : hz);
            ch[c2][i] = x + J->wet * wv;
        }
        J->w = (J->w + 1) & (VA_CHBUF - 1);
        J->env = mx > J->env ? mx : J->env * erel;
        if (J->env < 1e-12f) J->env = 0.f;
    }
    for (int c2 = 0; c2 < 2; c2++) {
        for (int k = 0; k < 2; k++) {
            if (fabsf(J->hs[c2][k]) < 1e-20f) J->hs[c2][k] = 0.f;
            if (fabsf(J->pre[c2][k]) < 1e-20f) J->pre[c2][k] = 0.f;
        }
        if (fabsf(J->post[c2]) < 1e-20f) J->post[c2] = 0.f;
    }
}

/* ------------------------------------------------------------------ presets */
#define P(...) {__VA_ARGS__, -1}
#define CH_I CHO, 1
#define CH_II CHO, 2
#define CH_III CHO, 3
static const char ARP1[] = "{\"mode\":0,\"rate\":4,\"oct\":1,\"gate\":40}";
static const char ARP2[] = "{\"mode\":2,\"rate\":4,\"oct\":2,\"gate\":35}";
static const char ARP3[] = "{\"mode\":1,\"rate\":4,\"oct\":1,\"gate\":45}";
static const char ARP4[] = "{\"mode\":6,\"rate\":5,\"oct\":1,\"gate\":40}";

static const va_preset PRE[] = {
    {"INIT", "Init", 0, NULL, P(CUT, 100)},
    /* ---- Bass */
    {"Aura Sub", "Bass", 0, NULL, P(RANGE, 0, SUB, 110, CUT, 46, RES, 24, ENV, 48, DEC, 42, SUS, 40, REL, 18, KEY, 40, AVEL, 50)},
    {"Rolling Psy", "Bass", 0, NULL, P(RANGE, 0, SUB, 64, CUT, 34, RES, 52, ENV, 70, DEC, 33, SUS, 0, REL, 14, KEY, 64, FVEL, 60, AVEL, 40, DRIFT, 6)},
    {"Pulse Thump", "Bass", 0, NULL, P(RANGE, 0, SAW, 0, PULSE, 1, PW, 46, SUB, 90, CUT, 42, RES, 38, ENV, 60, DEC, 39, SUS, 18, REL, 16, AVEL, 50)},
    {"Dub Pressure", "Bass", 0, NULL, P(RANGE, 0, SAW, 0, PULSE, 1, SUB, 120, CUT, 28, RES, 22, ENV, 30, DEC, 54, SUS, 70, REL, 26, HPF, 0, KEY, 30)},
    {"Acid Aura", "Bass", 0, NULL, P(RANGE, 0, CUT, 26, RES, 104, ENV, 82, DEC, 36, SUS, 0, REL, 15, FVEL, 90, KEY, 70, AVEL, 30)},
    /* ---- Lead */
    {"Silk Lead", "Lead", 0, NULL, P(PULSE, 1, PWMODE, 1, PW, 70, CUT, 86, RES, 28, ENV, 30, DEC, 52, SUS, 100, REL, 32, DLFO, 22, L1RATE, 94, L1DELAY, 46, GLIDE, 26, CH_I)},
    {"Square Hero", "Lead", 0, NULL, P(SAW, 0, PULSE, 1, SUB, 40, CUT, 80, RES, 22, ENV, 40, DEC, 50, SUS, 92, REL, 28, DLFO, 14, L1RATE, 96, L1DELAY, 56, CH_II)},
    {"Reso Whistle", "Lead", 0, NULL, P(CUT, 64, RES, 98, ENV, 56, DEC, 54, SUS, 70, REL, 31, KEY, 110, GLIDE, 20, CH_I, MS(2), S_AT, MD(2), JD_CUT, MA(2), 20)},
    {"Mono Glide", "Lead", 0, NULL, P(SUB, 50, CUT, 74, RES, 40, ENV, 50, DEC, 46, SUS, 80, REL, 27, GLIDE, 44, DLFO, 10, L1RATE, 92, L1DELAY, 70)},
    /* ---- Pad */
    {"Velvet Pad", "Pad", 0, NULL, P(PULSE, 1, PWMODE, 1, PW, 84, L1RATE, 66, CUT, 74, RES, 14, ENV, 18, ATT, 54, DEC, 61, SUS, 110, REL, 60, CH_II, FVEL, 0, AVEL, 20)},
    {"Dream Pad", "Pad", 0, NULL, P(SAW, 0, PULSE, 1, PWMODE, 1, PW, 96, L1RATE, 58, CUT, 70, RES, 30, ENV, 26, ATT, 67, DEC, 72, SUS, 96, REL, 68, CH_I, MS(2), S_LFO2, MD(2), JD_CUT, MA(2), 10, L2RATE, 30, FVEL, 0)},
    {"Ether Halo", "Pad", 0, NULL, P(HPF, 2, CUT, 72, RES, 36, ENV, 34, ATT, 72, DEC, 72, SUS, 84, REL, 74, CH_III, MS(2), S_LFO2, MD(2), JD_PW, MA(2), 30, PULSE, 1, PWMODE, 0, PW, 40, L2RATE, 40, FVEL, 0)},
    {"Night Choir", "Pad", 0, NULL, P(SAW, 0, PULSE, 1, PW, 96, NOISE, 22, CUT, 66, RES, 46, ENV, 20, KEY, 90, ATT, 59, DEC, 67, SUS, 100, REL, 65, CH_II, VLFO, 10, L1RATE, 70)},
    {"Slow Bloom", "Pad", 0, NULL, P(PULSE, 1, PWMODE, 2, PW, 90, SUB, 30, CUT, 48, RES, 26, ENV, 60, ATT, 79, DEC, 85, SUS, 70, REL, 71, CH_I, FVEL, 0, AVEL, 10)},
    /* ---- Strings */
    {"Aura Strings", "Strings", 0, NULL, P(HPF, 2, CUT, 88, ENV, 10, ATT, 44, DEC, 48, SUS, 120, REL, 49, CH_II, DLFO, 6, L1RATE, 92, L1DELAY, 60, FVEL, 0, AVEL, 30)},
    {"Analog Ensemble", "Strings", 0, NULL, P(PULSE, 1, PWMODE, 1, PW, 60, L1RATE, 74, HPF, 2, CUT, 84, ENV, 14, ATT, 40, DEC, 48, SUS, 116, REL, 47, CH_III, AVEL, 30)},
    {"Cinema Swell", "Strings", 0, NULL, P(HPF, 2, CUT, 60, RES, 10, ENV, 50, ATT, 69, DEC, 79, SUS, 90, REL, 63, CH_II, FVEL, 0, MS(2), S_WHEEL, MD(2), JD_CUT, MA(2), 30)},
    /* ---- Brass */
    {"Synth Brass", "Brass", 0, NULL, P(CUT, 54, RES, 16, ENV, 66, ATT, 26, DEC, 48, SUS, 90, REL, 28, FVEL, 50, AVEL, 40, CH_I)},
    {"Poly Brass 84", "Brass", 0, NULL, P(PULSE, 1, PW, 20, CUT, 58, RES, 10, ENV, 72, ATT, 23, DEC, 44, SUS, 86, REL, 27, FVEL, 60, CH_I, DLFO, 6, L1RATE, 90, L1DELAY, 70)},
    {"Neon Horns", "Brass", 0, NULL, P(SUB, 30, CUT, 50, RES, 30, ENV, 78, ATT, 21, DEC, 42, SUS, 84, REL, 26, FVEL, 70, CH_II)},
    /* ---- Keys */
    {"Aura Piano", "Keys", 0, NULL, P(SAW, 0, PULSE, 1, PW, 50, CUT, 58, RES, 6, ENV, 62, ATT, 0, DEC, 64, SUS, 0, REL, 40, AVEL, 80, FVEL, 60, CH_I)},
    {"Velvet EP", "Keys", 0, NULL, P(SAW, 0, PULSE, 1, PW, 30, SUB, 44, CUT, 54, RES, 10, ENV, 46, DEC, 67, SUS, 0, REL, 45, AVEL, 80, CH_II, MS(2), S_LFO2, MD(2), JD_LEVEL, MA(2), 12, L2RATE, 78)},
    {"Organ 60", "Keys", 0, NULL, P(SAW, 0, PULSE, 1, SUB, 90, CUT, 100, VCA, 1, ENV, 0, CH_III, FVEL, 0, AVEL, 0, REL, 14)},
    /* ---- Pluck */
    {"Aura Pluck", "Pluck", 0, NULL, P(CUT, 48, RES, 36, ENV, 74, DEC, 39, SUS, 0, REL, 29, FVEL, 50, CH_I)},
    {"Glass Pluck", "Pluck", 0, NULL, P(SAW, 0, PULSE, 1, PW, 100, HPF, 3, CUT, 60, RES, 62, ENV, 78, DEC, 37, SUS, 0, REL, 38, CH_II)},
    {"Wood Pluck", "Pluck", 0, NULL, P(SAW, 0, PULSE, 1, PW, 20, SUB, 50, CUT, 42, RES, 70, KEY, 127, ENV, 62, DEC, 33, SUS, 0, REL, 26)},
    /* ---- Arp (short psy plucks) */
    {"Psy Arp", "Arp", JF_ARP, ARP1, P(CUT, 44, RES, 56, ENV, 80, DEC, 31, SUS, 0, REL, 18, FVEL, 40, CH_I)},
    {"Trance Gate", "Arp", JF_ARP, ARP2, P(PULSE, 1, PW, 40, CUT, 50, RES, 46, ENV, 70, DEC, 33, SUS, 0, REL, 19, CH_II)},
    {"Goa Ripple", "Arp", JF_ARP, ARP3, P(SAW, 0, PULSE, 1, PWMODE, 1, PW, 70, L1RATE, 100, CUT, 40, RES, 70, ENV, 76, DEC, 30, SUS, 0, REL, 17, KEY, 90, CH_I)},
    {"Bubble Arp", "Arp", JF_ARP, ARP4, P(SAW, 0, PULSE, 1, PW, 90, CUT, 52, RES, 80, ENV, 60, DEC, 29, SUS, 0, REL, 16, KEY, 110, HPF, 2)},
    /* ---- Bell */
    {"Aura Bell", "Bell", 0, NULL, P(RANGE, 2, SAW, 0, PULSE, 1, PW, 110, CUT, 76, RES, 80, KEY, 127, ENV, 40, DEC, 69, SUS, 0, REL, 63, CH_III, AVEL, 60)},
    {"Ice Chime", "Bell", 0, NULL, P(RANGE, 2, HPF, 3, CUT, 82, RES, 92, KEY, 127, ENV, 30, DEC, 67, SUS, 0, REL, 65, CH_II, FVEL, 0)},
    /* ---- Drone */
    {"Deep Drone", "Drone", JF_HOLD, NULL, P(RANGE, 0, SUB, 80, CUT, 44, RES, 60, VLFO, 26, L1RATE, 34, ATT, 82, SUS, 127, REL, 77, CH_I, FVEL, 0, AVEL, 0, KEY, 30)},
    {"Healing Hum", "Drone", 0, NULL, P(SAW, 0, PULSE, 1, PWMODE, 1, PW, 70, L1RATE, 40, SUB, 60, CUT, 56, RES, 20, ATT, 72, SUS, 127, REL, 71, CH_III, FVEL, 0, AVEL, 0)},
    /* ---- Texture */
    {"Ocean Wash", "Texture", 0, NULL, P(SAW, 0, NOISE, 127, CUT, 66, RES, 72, VLFO, 64, L1RATE, 40, ATT, 72, SUS, 127, REL, 77, CH_II, KEY, 127, FVEL, 0)},
    {"Breath Field", "Texture", 0, NULL, P(NOISE, 70, CUT, 58, RES, 50, ENV, 30, ATT, 61, DEC, 72, SUS, 90, REL, 65, CH_III, MS(2), S_LFO2, MD(2), JD_CUT, MA(2), 14, L2RATE, 50)},
    /* ---- FX */
    {"Laser Drop", "FX", 0, NULL, P(CUT, 110, RES, 100, ENV, 127, ENVPOL, 1, ATT, 0, DEC, 52, SUS, 0, REL, 35, KEY, 100, CH_I)},
    {"Siren Sweep", "FX", 0, NULL, P(SAW, 0, PULSE, 1, DLFO, 70, L1RATE, 76, CUT, 76, RES, 60, VLFO, 40, SUS, 127, REL, 43, CH_II)},
    /* ---- Sequence */
    {"Seq Pulse", "Sequence", 0, NULL, P(SAW, 0, PULSE, 1, PW, 50, CUT, 52, RES, 60, ENV, 56, DEC, 30, SUS, 0, REL, 14, CH_I, FVEL, 60)},
    {"Synthwave Seq", "Sequence", 0, NULL, P(RANGE, 0, SUB, 60, CUT, 50, RES, 30, ENV, 62, DEC, 34, SUS, 10, REL, 15, CH_II, AVEL, 50)},
    /* ---- chord accompaniment (Chordially): classic chorus-poly chord sounds */
    {"Poly Chord Pad", "Pad", 0, NULL, P(PULSE, 1, PWMODE, 1, PW, 80, L1RATE, 60, SUB, 30, CUT, 70, RES, 18, ENV, 22, ATT, 50, DEC, 64, SUS, 110, REL, 58, CH_II, FVEL, 0)},
    {"Warm Chord Pad", "Pad", 0, NULL, P(SUB, 50, CUT, 58, RES, 10, ENV, 30, ATT, 44, DEC, 70, SUS, 100, REL, 60, CH_I, AVEL, 20)},
    {"Analog Choir", "Pad", 0, NULL, P(SAW, 0, PULSE, 1, PWMODE, 1, PW, 100, L1RATE, 52, NOISE, 14, HPF, 2, CUT, 64, RES, 40, KEY, 80, ATT, 56, DEC, 64, SUS, 110, REL, 60, CH_III, FVEL, 0)},
    {"Sunset Pad", "Pad", 0, NULL, P(PULSE, 1, PWMODE, 2, PW, 100, CUT, 54, RES, 26, ENV, 50, ATT, 70, DEC, 84, SUS, 80, REL, 66, CH_I)},
    {"Classic Ensemble", "Strings", 0, NULL, P(PULSE, 1, PWMODE, 1, PW, 70, L1RATE, 70, HPF, 2, CUT, 86, RES, 8, ENV, 8, ATT, 46, DEC, 50, SUS, 122, REL, 52, CH_III, DLFO, 4, L1DELAY, 60, FVEL, 0)},
    {"Soft Brass Pad", "Brass", 0, NULL, P(CUT, 50, RES, 12, ENV, 58, ATT, 34, DEC, 56, SUS, 96, REL, 44, FVEL, 50, CH_II)},
    {"House Organ Stab", "Keys", 0, NULL, P(SAW, 0, PULSE, 1, PW, 30, SUB, 80, CUT, 78, RES, 20, ENV, 40, ATT, 0, DEC, 50, SUS, 30, REL, 24, CH_III, FVEL, 40)},
    {"80s Poly Keys", "Keys", 0, NULL, P(PULSE, 1, PW, 40, CUT, 62, RES, 14, ENV, 56, DEC, 60, SUS, 40, REL, 36, CH_I, AVEL, 60, FVEL, 50)},
    {"Lofi Keys", "Keys", 0, NULL, P(SAW, 0, PULSE, 1, PW, 64, SUB, 40, CUT, 44, RES, 8, ENV, 40, DEC, 62, SUS, 20, REL, 46, CH_II, HISS, 90, AVEL, 70, DLFO, 3, L1RATE, 40)},
    {"Glass Poly", "Keys", 0, NULL, P(SAW, 0, PULSE, 1, PW, 110, HPF, 2, CUT, 72, RES, 60, KEY, 120, ENV, 36, DEC, 66, SUS, 20, REL, 54, CH_II, AVEL, 50)},
    {"Gospel Organ", "Keys", 0, NULL, P(PULSE, 1, PW, 64, SUB, 100, CUT, 110, ENV, 0, VCA, 1, CH_III, REL, 18, FVEL, 0, AVEL, 0)},
    {"Chord Stab", "Pluck", 0, NULL, P(SUB, 40, CUT, 46, RES, 52, ENV, 72, DEC, 40, SUS, 0, REL, 26, CH_I, FVEL, 50)},
};
#define NPRE ((int)(sizeof PRE / sizeof PRE[0]))

#if __has_include("va_trims.h")
#include "va_trims.h"
#define JU_TRIMS TRIM_AURA
#else
#define JU_TRIMS NULL
#endif

const va_eng VA_ENG_AURA = {
    .id = "aura", .npages = (int)(sizeof PAGES / sizeof PAGES[0]), .pages = PAGES, .ns = NS, .ps = PS,
    .ndest = (int)(sizeof DNAME / sizeof DNAME[0]), .dname = DNAME, .did = DID, .npre = NPRE, .pre = PRE, .mac = MAC,
    .trim = JU_TRIMS, .poly = 16,
    .envp = {{ATT, DEC, SUS, REL}, {ATT, DEC, SUS, REL}}, .envkey = {-1, -1}, .p_vcagate = VCA, .p_cutoff = CUT,
    .p_envtrig = -1, .p_porta = -1, .p_legato = -1,
    .voicing = NULL, .von = ju_von, .vctrl = ju_vctrl, .vdsp = ju_vdsp, .post = ju_post, .post_reset = ju_post_reset,
};
static void *ju_create(double sr) { return va_create(&VA_ENG_AURA, sr); }
const jeng_def_t JE_AURA = {.id = "aura", .name = "AURA", .family = "Analog", .poly = 16, VA_JENG_FIELDS(ju_create)};
