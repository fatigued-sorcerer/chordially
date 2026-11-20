// SPDX-License-Identifier: GPL-3.0-only
// Jammer: FATHOM, a wide, fat vintage polysynth (16 voices).
// Per voice: OSC 1 (saw, pulse) and OSC 2 (saw, pulse, hard sync to OSC 1, X-MOD = OSC 1 -> OSC 2 audio-rate FM,
// DETUNE), noise -> mixer -> WARMTH (soft drive) -> 12 dB/oct state-variable filter with the SEM's continuous
// LP - NOTCH - HP morph (MODE) and a BP switch; the band state is softly limited, so the resonance sings without
// turning harsh -> VCA -> per-voice stereo pan (each voice slot has its own pan position like classic wide polysynths,
// SPREAD sets the width). DETUNE spreads UNISON stacks (up to 8 voices) and also gives every voice in POLY a fixed
// small offset (the classic per-voice detune) -- wide and fat. SLOP adds per-voice analog variation and drift.
// LFO 1 has its own TO PITCH / TO PWM / TO CUTOFF amounts and WHEEL (vibrato depth added by the mod wheel);
// LFO 2 and the 8-slot matrix for the rest.
typedef struct vx {
    float p1, p2, y2prev;
    float dt1, dt1d, dt2, dt2d;
    float pw1, pw1d, pw2, pw2d;
    float g, gd, k, kd, ic1, ic2;
    float lpw, hpw, bpw;               /* morph weights */
    float drv, dn;                     /* warmth drive + makeup */
    float amp, ampd, gl, gr;
    float l1, l2, ln, xm;
    int s1, q1, s2, q2, sync, first;
} vx_t;
typedef struct gx { int dummy; } gx_t;
#include "va2_common.h"

enum { PG_OSC1, PG_OSC2, PG_MIX, PG_FILT, PG_FENV, PG_AENV, PG_LFO1, PG_LFO2, PG_VOICE, PG_CHAR, PG_LEVEL, PG_MOD1,
       NPG = PG_MOD1 + 8 };
static const char *const PAGES[NPG] = {"OSC 1", "OSC 2", "MIXER", "FILTER", "FILTER ENV", "AMP ENV", "LFO 1", "LFO 2",
                                       "VOICE", "CHARACTER", "LEVEL", "MOD 1", "MOD 2", "MOD 3", "MOD 4", "MOD 5",
                                       "MOD 6", "MOD 7", "MOD 8"};
enum {
    O1_FREQ, O1_SAW, O1_PULSE, O1_PW,
    O2_FREQ, O2_DET, O2_SAW, O2_PULSE, O2_PW, O2_SYNC, O2_XMOD,
    MX_1, MX_2, MX_N,
    F_CUT, F_RES, F_MODE, F_BP, F_ENV, F_KEY,
    FE_A, AE_A = FE_A + VA_ENV_N,
    L1_W = AE_A + VA_ENV_N, L1_PITCH = L1_W + VA_LFO_N, L1_PWM, L1_CUT, L1_WHEEL,
    L2_W,
    V_MODE = L2_W + VA_LFO_N, V_STACK, V_DETUNE, V_GLIDE, V_LEGATO, V_BEND,
    C_WARM, C_SLOP,
    LV_LEVEL, LV_PAN, LV_SPREAD,
    MOD1, NP = MOD1 + VA_MOD_N
};
static const va_ps PS[NP] = {
    [O1_FREQ] = PK("FREQ", PG_OSC1, -24, 24, 0, F_SEMI, 1), PT("SAW", PG_OSC1, 1, 1), PT("PULSE", PG_OSC1, 0, 1),
    PK("PW", PG_OSC1, 0, 127, 64, F_PW, 1),
    [O2_FREQ] = PK("FREQ", PG_OSC2, -24, 24, 0, F_SEMI, 1), PK("DETUNE", PG_OSC2, -50, 50, 8, F_CENT, 1),
    PT("SAW", PG_OSC2, 1, 1), PT("PULSE", PG_OSC2, 0, 1), PK("PW", PG_OSC2, 0, 127, 64, F_PW, 0),
    PT("SYNC", PG_OSC2, 0, 1), PK("X-MOD", PG_OSC2, 0, 127, 0, F_PCT, 0),
    [MX_1] = PK("OSC 1", PG_MIX, 0, 127, 100, F_PCT, 1), PK("OSC 2", PG_MIX, 0, 127, 100, F_PCT, 1),
    PK("NOISE", PG_MIX, 0, 127, 0, F_PCT, 1),
    [F_CUT] = PK("CUTOFF", PG_FILT, 0, 127, 82, F_CUTOFF, 1), PK("RESO", PG_FILT, 0, 127, 24, F_PCT, 1),
    PK("MODE", PG_FILT, 0, 127, 0, F_MORPH, 1), PT("BP", PG_FILT, 0, 1), PK("ENV AMT", PG_FILT, -64, 63, 24, F_BIP, 1),
    PK("KEYTRACK", PG_FILT, 0, 127, 64, F_PCT, 0),
    [FE_A] = VA_ENV_PS(PG_FENV, 9, 44, 50, 33, 40, 1),
    [AE_A] = VA_ENV_PS(PG_AENV, 10, 41, 127, 29, 45, 1),
    [L1_W] = VA_LFO_PS(PG_LFO1, L1_W, 0, 86, 1), PK("TO PITCH", PG_LFO1, 0, 127, 0, F_PCT, 1),
    PK("TO PWM", PG_LFO1, 0, 127, 0, F_PCT, 1), PK("TO CUTOFF", PG_LFO1, 0, 127, 0, F_PCT, 1),
    PK("WHEEL", PG_LFO1, 0, 127, 64, F_PCT, 0),
    [L2_W] = VA_LFO_PS(PG_LFO2, L2_W, 1, 40, 0),
    [V_MODE] = PC("VOICES", PG_VOICE, VA_VOICEMODE, 0, 1), PK("STACK", PG_VOICE, 2, 8, 8, F_VOICES, 0),
    PK("DETUNE", PG_VOICE, 0, 127, 36, F_PCT, 1), PK("GLIDE", PG_VOICE, 0, 127, 0, F_GLIDE, 1),
    PC("LEGATO", PG_VOICE, VA_LEGATO, 0, 0), PK("BEND RANGE", PG_VOICE, 1, 24, 2, F_ST, 0),
    [C_WARM] = PK("WARMTH", PG_CHAR, 0, 127, 40, F_PCT, 1), PK("SLOP", PG_CHAR, 0, 127, 32, F_PCT, 0),
    [LV_LEVEL] = PK("LEVEL", PG_LEVEL, 0, 127, 100, F_LEVEL, 1), PK("PAN", PG_LEVEL, -64, 63, 0, F_PAN, 0),
    PK("SPREAD", PG_LEVEL, 0, 127, 64, F_PCT, 1),
    [MOD1] = VA_MOD_PS(PG_MOD1), VA_MOD_PS(PG_MOD1 + 1), VA_MOD_PS(PG_MOD1 + 2), VA_MOD_PS(PG_MOD1 + 3),
    VA_MOD_PS(PG_MOD1 + 4), VA_MOD_PS(PG_MOD1 + 5), VA_MOD_PS(PG_MOD1 + 6), VA_MOD_PS(PG_MOD1 + 7),
};
static const signed char SRC[] = {S_OFF, S_LFO1, S_LFO2, S_FENV, S_AENV, S_VEL, S_NOTE, S_WHEEL, S_AT, S_RAND, S_BEND, S_STACK};
enum { MS_OFF, MS_LFO1, MS_LFO2, MS_FENV, MS_AENV, MS_VEL, MS_NOTE, MS_WHEEL, MS_AT, MS_RAND, MS_BEND, MS_STACK };
enum { D_OFF, D_PITCH, D_P1, D_P2, D_PW1, D_PW2, D_LV1, D_LV2, D_LVN, D_XMOD, D_CUT, D_RES, D_MODE, D_FENV, D_WARM,
       D_AMP, D_PAN, D_L1RATE, D_L2RATE, D_L1AMT, D_L2AMT, D_ATK, D_DEC, D_REL, ND };
static const char *const DST[ND] = {"OFF", "PITCH", "OSC 1 PITCH", "OSC 2 PITCH", "PW 1", "PW 2", "OSC 1 LVL",
                                    "OSC 2 LVL", "NOISE LVL", "X-MOD", "CUTOFF", "RESO", "MODE", "FENV AMT", "WARMTH",
                                    "AMP", "PAN", "LFO 1 RATE", "LFO 2 RATE", "LFO 1 AMT", "LFO 2 AMT", "ATTACK",
                                    "DECAY", "RELEASE"};
#define MOD(k, s, d, a) MOD1 + 3 * ((k) - 1), s, MOD1 + 3 * ((k) - 1) + 1, d, MOD1 + 3 * ((k) - 1) + 2, a

static const va_macro MACROS[8] = {
    {"CUTOFF", 2, {{F_CUT, 50, 0}, {F_MODE, 30, 0}}},   /* + leans the morph toward HP, - toward LP (audible on notch presets) */
    {"RESO", 1, {{F_RES, 45, 0}}},
    {"ENV AMT", 2, {{F_ENV, 60, 0}, {F_CUT, -15, 0}}},   /* more envelope sweeps from a darker base */
    {"ATTACK", 2, {{AE_A, 112, 0}, {FE_A, 100, 0}}},
    {"DECAY", 4, {{AE_A + 1, 112, 0}, {FE_A + 1, 100, 0}, {AE_A + 2, -25, 1}, {AE_A + 2, 80, -1}}},   /* - half: pluckier (sustain sinks) */
    {"RELEASE", 4, {{AE_A + 3, 112, 0}, {FE_A + 3, 100, 0}, {AE_A + 1, 40, 0}, {FE_A + 1, 30, 0}}},        /* + decay: plucks ring longer */
    {"MODE", 1, {{F_MODE, 130, 0}}},
    {"FAT", 4, {{C_WARM, 40, 0}, {V_DETUNE, 35, 0}, {LV_SPREAD, 40, 0}, {O2_DET, 12, 0}}},
};

#include "fathom_presets.h"

static float naive2(const vx_t *x, float p, float pw) {
    float y = 0.f;
    if (x->s2) y += va_saw_n(p);
    if (x->q2) y += va_pulse_n(p, pw);
    return y;
}

static void ob_vstart(va *e, va_voice *v, int fresh) {
    vx_t *x = &v->x;
    (void)e;
    if (fresh) {
        x->ic1 = x->ic2 = 0.f;
        x->p1 = va_noise(&v->rng) * 0.5f + 0.5f; x->p2 = va_noise(&v->rng) * 0.5f + 0.5f;
        x->y2prev = va_saw_n(x->p2);
        x->amp = x->ampd = 0.f; x->dt1 = x->dt2 = 0.f; x->g = 0.f; x->k = 2.f; x->first = 1;
    }
}

static void ob_vtick(va *e, va_voice *v, int n) {
    vx_t *x = &v->x;
    const float *ps = e->ps; const int *ef = e->eff; const float *m = v->mod;
    float slop = ps[C_SLOP] * (1.f / 127.f);
    x->s1 = ef[O1_SAW]; x->q1 = ef[O1_PULSE]; x->s2 = ef[O2_SAW]; x->q2 = ef[O2_PULSE]; x->sync = ef[O2_SYNC];
    if (!x->s2 && !x->q2) x->sync = 0;
    float lfo = v->lfo[0];
    float lp = ps[L1_PITCH] * (1.f / 127.f);
    float vib = lp * lp * 12.f + e->wheel_s * ps[L1_WHEEL] * (1.f / 127.f) * 0.6f;
    float pitch = 48.f * m[D_PITCH] + lfo * vib;
    float p1 = v->pbase + (float)ef[O1_FREQ] + pitch + 48.f * m[D_P1];
    float p2 = v->pbase + (float)ef[O2_FREQ] + ps[O2_DET] * 0.01f + pitch + 48.f * m[D_P2] + slop * 0.06f * v->vr[4];
    va_ramp0(&x->dt1, &x->dt1d, va_tune_dt(e, p1), n, x->first);
    va_ramp0(&x->dt2, &x->dt2d, va_tune_dt(e, p2), n, x->first);
    float pwm = ps[L1_PWM] * (1.f / 127.f) * 0.42f * lfo;
    float pw1 = 0.05f + 0.9f * ps[O1_PW] * (1.f / 127.f) + pwm + 0.45f * m[D_PW1] + slop * 0.03f * v->vr[3];
    float pw2 = 0.05f + 0.9f * ps[O2_PW] * (1.f / 127.f) - pwm + 0.45f * m[D_PW2] - slop * 0.03f * v->vr[3];
    va_ramp0(&x->pw1, &x->pw1d, va_clampf(pw1, 0.03f, 0.97f), n, x->first);
    va_ramp0(&x->pw2, &x->pw2d, va_clampf(pw2, 0.03f, 0.97f), n, x->first);
    float l1 = va_clampf(ps[MX_1] * (1.f / 127.f) + m[D_LV1], 0.f, 1.2f);
    float l2 = va_clampf(ps[MX_2] * (1.f / 127.f) + m[D_LV2], 0.f, 1.2f);
    float ln = va_clampf(ps[MX_N] * (1.f / 127.f) + m[D_LVN], 0.f, 1.2f);
    x->l1 = l1 * l1 * 0.5f; x->l2 = l2 * l2 * 0.5f; x->ln = ln * ln * 0.45f;
    float xm = va_clampf(ps[O2_XMOD] * (1.f / 127.f) + m[D_XMOD], 0.f, 1.f);
    x->xm = xm * xm * 4.f;
    /* filter */
    float fenv = v->env[1].v * va_velgain(v->vel, ps[FE_A + 4] * (1.f / 127.f));
    float envamt = va_clampf(ps[F_ENV] * (1.f / 64.f) + m[D_FENV], -1.f, 1.f);
    float lc = ps[L1_CUT] * (1.f / 127.f);
    float cs = va_cut_st(ps[F_CUT]) + envamt * fenv * 80.f + ps[F_KEY] * (1.f / 127.f) * (v->pbase - 60.f)
             + lc * lc * 36.f * lfo + 72.f * m[D_CUT] + slop * 3.f * v->vr[1];
    va_ramp0(&x->g, &x->gd, va_cut_g(e, cs), n, x->first);
    float res = va_clampf(ps[F_RES] * (1.f / 127.f) + m[D_RES], 0.f, 1.f);
    va_ramp0(&x->k, &x->kd, 2.f - 1.95f * res * (2.f - res), n, x->first);
    float mode = va_clampf(ps[F_MODE] * (1.f / 127.f) + m[D_MODE], 0.f, 1.f);
    if (ef[F_BP]) {   /* BP: the MODE macro / matrix offset morphs BP toward LP (-) or HP (+); the BP preset itself is exact */
        float d = va_clampf((float)e->add[F_MODE] * (1.f / 127.f) + m[D_MODE], -1.f, 1.f);
        x->bpw = 1.f - fabsf(d); x->lpw = d < 0.f ? -d : 0.f; x->hpw = d > 0.f ? d : 0.f;
    }
    else { x->lpw = mode < 0.5f ? 1.f : 2.f - 2.f * mode; x->hpw = mode > 0.5f ? 1.f : 2.f * mode; x->bpw = 0.f; }
    float w = va_clampf(ps[C_WARM] * (1.f / 127.f) + m[D_WARM], 0.f, 1.f);
    x->drv = 0.7f + 3.3f * w * w;
    x->dn = 1.f / va_tanh(x->drv * 0.6f) * 0.6f;        /* a 0.6 peak keeps its level through the drive */
    float vg = va_velgain(v->vel, ps[AE_A + 4] * (1.f / 127.f));
    float amp = v->env[0].v * vg * v->sgain * va_clampf(1.f + m[D_AMP], 0.f, 2.f) * (1.f + slop * 0.08f * v->vr[2]);
    if (v->env[0].st == 0) amp = 0.f;
    va_ramp(&x->amp, &x->ampd, amp * 0.62f, n);
    float pan = va_clampf(v->pan + m[D_PAN], -1.f, 1.f);
    /* equal-power-ish pan: centre = 0.71 each side would drop mono; keep centre at unity, sides to 0 / 1.2 */
    x->gl = pan > 0.f ? 1.f - pan : 1.f + 0.2f * -pan;
    x->gr = pan < 0.f ? 1.f + pan : 1.f + 0.2f * pan;
    x->first = 0;
}

static void ob_vrender(va *e, va_voice *v, float *L, float *R, int n) {
    vx_t *x = &v->x;
    float p1 = x->p1, p2 = x->p2, yprev = x->y2prev, ic1 = x->ic1, ic2 = x->ic2;
    float dt1 = x->dt1, dt2 = x->dt2, pw1 = x->pw1, pw2 = x->pw2, g = x->g, k = x->k, amp = x->amp;
    const float l1 = x->l1, l2 = x->l2, ln = x->ln, xm = x->xm, drv = x->drv, dn = x->dn;
    const float lpw = x->lpw, hpw = x->hpw, bpw = x->bpw, gl = x->gl, gr = x->gr;
    const int s1 = x->s1, q1 = x->q1, s2 = x->s2, q2 = x->q2, sync = x->sync;
    const float dt1d = x->dt1d, dt2d = x->dt2d, pw1d = x->pw1d, pw2d = x->pw2d, gd = x->gd, kd = x->kd, ampd = x->ampd;
    uint32_t rng = v->rng;
    (void)e;
    for (int i = 0; i < n; i++) {
        p1 += dt1; int wr = 0;
        if (p1 >= 1.f) { p1 -= 1.f; wr = 1; }
        float y1 = 0.f;
        if (s1) y1 += va_saw(p1, dt1);
        if (q1) y1 += va_pulse(p1, dt1, pw1);
        float d2 = dt2;
        if (xm != 0.f) { d2 *= va_exp2(y1 * xm); if (d2 > 0.45f) d2 = 0.45f; }
        p2 += d2;
        if (p2 >= 1.f) p2 -= floorf(p2);
        float y2;
        if (sync && wr) {
            float d = p1 / dt1; if (d > 1.f) d = 1.f;
            float pr = p2 - d * d2; if (pr < 0.f) pr += 1.f;
            float h = naive2(x, 0.f, pw2) - naive2(x, pr, pw2);
            p2 = d * d2;
            y2 = naive2(x, p2, pw2) - h * 0.5f * (1.f - d) * (1.f - d);
            yprev += h * 0.5f * d * d;
        } else {
            y2 = 0.f;
            if (s2) y2 += va_saw(p2, d2);
            if (q2) y2 += va_pulse(p2, d2, pw2);
        }
        float o2 = yprev; yprev = y2;
        float in = va_tanh((y1 * l1 + o2 * l2 + (ln != 0.f ? va_noise(&rng) * ln : 0.f)) * drv) * dn;
        /* SEM 12 dB state variable (TPT), band state softly limited */
        float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float v3 = in - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
        if (ic1 > 1.5f || ic1 < -1.5f) ic1 = 1.5f * va_tanh(ic1 * (1.f / 1.5f));
        float hp = in - k * v1 - v2;
        float y = lpw * v2 + hpw * hp + bpw * v1 * (k + 0.3f);
        float o = y * amp;
        L[i] = o * gl; R[i] = o * gr;
        dt1 += dt1d; dt2 += dt2d; pw1 += pw1d; pw2 += pw2d; g += gd; k += kd; amp += ampd;
    }
    v->rng = rng;
    x->p1 = p1; x->p2 = p2; x->y2prev = yprev; x->ic1 = va_flush(ic1); x->ic2 = va_flush(ic2);
    x->dt1 = dt1; x->dt2 = dt2; x->pw1 = pw1; x->pw2 = pw2; x->g = g; x->k = k; x->amp = amp;
}

static const va_spec SPEC = {
    .np = NP, .ps = PS, .npages = NPG, .pages = PAGES, .macros = MACROS,
    .npre = (int)(sizeof PRESETS / sizeof PRESETS[0]), .pre = PRESETS,
    .nsrc = (int)sizeof SRC, .src = SRC, .ndst = ND, .dst = DST,
    .nenv = 2, .nlfo = 2,
    .p_env = {AE_A, FE_A, -1}, .p_lfo = {L1_W, L2_W, -1}, .p_mod = MOD1,
    .p_voices = V_MODE, .p_stack = V_STACK, .p_detune = V_DETUNE, .p_glide = V_GLIDE, .p_legato = V_LEGATO,
    .p_bend = V_BEND, .p_level = LV_LEVEL, .p_pan = LV_PAN, .p_spread = LV_SPREAD, .p_slop = C_SLOP, .p_cutoff = F_CUT,
    .d_lforate = {D_L1RATE, D_L2RATE, -1}, .d_lfoamt = {D_L1AMT, D_L2AMT, -1}, .d_atk = D_ATK, .d_dec = D_DEC, .d_rel = D_REL,
    .dquad = (1ull << D_PITCH) | (1ull << D_P1) | (1ull << D_P2),
    .poly_det = 0.3f,
    .vstart = ob_vstart, .vtick = ob_vtick, .vrender = ob_vrender,
};
static void *ob_create(double sr) { return va_create(&SPEC, sr); }
int fathom_debug_voices(void *e, int *held, int *rel, int *fade) { return va_dbg(e, held, rel, fade); }

const jeng_def_t JE_FATHOM = {
    .id = "fathom", .name = "FATHOM", .family = "Analog",
    VA_JENG_FIELDS(ob_create, VA_POLY),
};
