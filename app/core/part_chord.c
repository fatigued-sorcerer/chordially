// SPDX-License-Identifier: GPL-3.0-only
// Chordially: the CHORD part. One key = one chord (CHORD on) from the chord set, or the held notes themselves
// (CHORD off). The chord sounds as a phrase (STYLE / VARIATION) or held (STYLE OFF), through the AURA synth and / or
// MIDI out. The chord sequencer: 64 patterns (8 banks x 8), up to 64 steps; a pattern also holds its sound, tempo,
// style, chord set, key and effects, like the J-6.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cm_int.h"
#include "chords.h"
#include "phrases.h"
#include "jeng.h"

extern const jeng_def_t JE_AURA, JE_TITAN, JE_FATHOM;
/* the internal synths: AURA (chorus poly, one DCO), TITAN (two VCOs, sync, separate filter / amp envelopes),
   FATHOM (two detuned VCOs, morphing LP / notch / HP filter). Only their chord-friendly presets are offered. */
static const jeng_def_t *const ENG[] = {&JE_AURA, &JE_TITAN, &JE_FATHOM};
static const char *const ENG_TAG[] = {"AURA", "TITAN", "FATHOM"};
#define NENG 3
#define NE 8   /* the SOUND knobs that set the sound's own parameters */
static const char *const KEEP_CAT[] = {"Pad", "Strings", "Brass", "Keys", "Pluck", "Bell", 0};

enum { NPAT = 64, NSTEP = 64 };
typedef struct { signed char n, tie, style, var; unsigned char note[4]; char label[14]; } step_t;   /* style -1: the pattern's */
typedef struct {
    char name[16];
    int last, steplen, bpm10, set, key, style, var, sound, cut, res, gate, swing, dly, rev, vol, center, used, grnd, ea, ed, es, er, eg, eql, eqm, eqh, pan, sub;   /* ea..eg, cut, res, sub: knob 0..127 on the sound's own parameter (-1 = the sound's own value) */
    step_t st[NSTEP];
} pat_t;

static const int STEPLEN[] = {24, 32, 48, 96, 192, 384};   /* 1/16, 1/8T, 1/8, 1/4, 1/2, 1 bar */
static const char *const STEPNAME[] = {"1/16", "1/8T", "1/8", "1/4", "1/2", "1 BAR"};
#define NSTEPLEN 6

typedef struct {
    void *eng; const jeng_def_t *E;
    int npre, pre[128];           /* the sounds: the engines' chord-friendly presets */
    pat_t pat[NPAT];
    int cur, next;                /* pattern now; queued (-1 none) */
    /* part settings (global) */
    int chordmode, outch, inch, local, strum_ms, oct, pchange;   /* pchange 0 END, 1 STEP, 2 BAR */
    char outport[96];
    /* live */
    int held[16], nheld, sustain, sus_chord, hold;   /* hold: HOLD (latch) button */
    int live_n, live_note[4]; char live_label[24];  /* the chord from the keys */
    int seq_n, seq_note[4]; char seq_label[24]; int seq_style, seq_var;
    int cur_n, cur_note[4]; char cur_label[24]; int cur_live;   /* what sounds now */
    int block_n, block_note[8];   /* STYLE OFF: the notes held as a block */
    long ph0;                     /* the phrase's first tick */
    int ph_restart;
    int step, sel, rec, edit;     /* playing step; step-edit cursor */
    long seq0;
    int cnt[128];                 /* sounding notes (synth + MIDI out) */
    int vel_live;
    unsigned rnd;
    int pending_change;           /* the playing pattern changed: apply on the next step */
    int gen;                      /* GENERATIVE: evolve the pattern each time round */
    int pidx[NE];                 /* the engine's ATTACK, DECAY, SUSTAIN, RELEASE, filter ENV amount, CUTOFF, RESO, SUB (found by name; -1 = none) */
    void *inst[NENG]; int ce;     /* every engine's instance; the one in use */
    int lvidx;                    /* the engine's LEVEL (chords: 10 dB below the single-note level, clear of its limiter) */
    int snd_eng[128];             /* sound i = engine snd_eng[i], preset pre[i], level snd_gain[i] */
    float snd_gain[128];
    int engine;                   /* 0 INTERNAL (AURA), 1 an AU instrument (the shell renders it) */
    int ev[4 * 512], nev;         /* this block's notes for the AU instrument: frame, status, d1, d2 */
} cs_t;
static cs_t C;
static void gen_mutate(void);

/* ------------------------------------------------------------------ notes out */
static void synth_midi(int frame, int st, int d1, int d2)
{
    if (C.engine == 0) { C.E->midi(C.eng, frame, st, d1, d2); return; }
    if (C.nev < 512) { int *e = C.ev + 4 * C.nev++; e[0] = frame; e[1] = st; e[2] = d1; e[3] = d2; }
}
static void out_on(int frame, int n, int v)
{
    if (n < 0 || n > 127) return;
    if (C.cnt[n] > 0) {                                   /* (a re-struck note: end the old one first) */
        if (C.local) synth_midi(frame, 0x80, n, 0);
        if (C.outport[0]) cm_midi_port(C.outport, frame, 0x80 | (C.outch - 1), n, 0);
    }
    C.cnt[n]++;
    if (C.local) synth_midi(frame, 0x90, n, v);
    if (C.outport[0]) cm_midi_port(C.outport, frame, 0x90 | (C.outch - 1), n, v);
}
static void out_off(int frame, int n)
{
    if (n < 0 || n > 127 || C.cnt[n] <= 0) return;
    if (--C.cnt[n] > 0) return;
    if (C.local) synth_midi(frame, 0x80, n, 0);
    if (C.outport[0]) cm_midi_port(C.outport, frame, 0x80 | (C.outch - 1), n, 0);
}
static void all_off(int frame)
{
    cm_unsched(&PART[0]);
    for (int n = 0; n < 128; n++) if (C.cnt[n] > 0) { C.cnt[n] = 1; out_off(frame, n); }
    C.block_n = 0;
}
static pat_t *P(void) { return &C.pat[C.cur]; }
static void all_off(int frame);
static void find_params(void)
{
    static const char *const NAME[4] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
    for (int k = 0; k < NE; k++) C.pidx[k] = -1;
    C.lvidx = -1;
    int np = C.E->nparams(C.eng);
    for (int i = 0; i < np; i++) {
        const char *pg = C.E->page_name(C.eng, C.E->param_page(C.eng, i)), *nm = C.E->param_name(C.eng, i);
        for (int k = 0; k < 4; k++) if (C.pidx[k] < 0 && !strcmp(pg, "AMP ENV") && !strcmp(nm, NAME[k])) C.pidx[k] = i;
        if (C.pidx[4] < 0 && !strcmp(pg, "FILTER") && (!strcmp(nm, "ENV") || !strcmp(nm, "ENV AMT"))) C.pidx[4] = i;
        if (C.pidx[5] < 0 && !strcmp(pg, "FILTER") && !strcmp(nm, "CUTOFF")) C.pidx[5] = i;
        if (C.pidx[6] < 0 && !strcmp(pg, "FILTER") && (!strcmp(nm, "RESO") || !strcmp(nm, "RESONANCE"))) C.pidx[6] = i;
        if (C.pidx[7] < 0 && !strcmp(pg, "MIXER") && !strcmp(nm, "SUB")) C.pidx[7] = i;
        if (C.lvidx < 0 && !strcmp(pg, "LEVEL") && !strcmp(nm, "LEVEL")) C.lvidx = i;
    }
}
/* another internal synth: the old one falls silent, the new one takes over (same notes from now on) */
/* each sound's level, so a held chord peaks near -1.5 dBFS on every sound (measured by tests/snd_calib.c) */
static const struct { const char *k; float g; } SND_LEVEL[] = {
#include "snd_levels.h"
    {0, 0}
};
static void snd_levels(void)
{
    for (int i = 0; i < C.npre; i++) {
        int e = C.snd_eng[i]; char k[96]; snprintf(k, sizeof k, "%s/%s", ENG_TAG[e], ENG[e]->preset_name(C.inst[e], C.pre[i]));
        for (int j = 0; SND_LEVEL[j].k; j++) if (!strcmp(SND_LEVEL[j].k, k)) { C.snd_gain[i] = SND_LEVEL[j].g; break; }
    }
}
int chord_sound_count(void) { return C.npre; }
static void engine_switch(int e)
{
    if (e < 0 || e >= NENG || e == C.ce) return;
    all_off(0);
    C.E->reset(C.eng);
    C.ce = e; C.E = ENG[e]; C.eng = C.inst[e];
    find_params();
}
/* ATTACK .. EG INT, FILTER, RESO, SUB: the sound's own parameters on a 0..127 knob (-1 = as the sound has it).
   The knob covers the usable part of each parameter: never a negative half (a bipolar EG INT shows 0..+max),
   FILTER from ~80 Hz up, RESO up to 90 % (no screaming self-oscillation). */
static int env_base[NE];
static int *env_field(int k) { pat_t *t = P(); return k == 0 ? &t->ea : k == 1 ? &t->ed : k == 2 ? &t->es : k == 3 ? &t->er : k == 4 ? &t->eg : k == 5 ? &t->cut : k == 6 ? &t->res : &t->sub; }
static void krange(int k, int *klo, int *khi)
{
    int i = C.pidx[k], lo = C.E->param_min(C.eng, i), hi = C.E->param_max(C.eng, i);
    if (lo < 0) lo = 0;
    if (k == 5) lo += (hi - lo) * 28 / 127;
    if (k == 6) hi = lo + (hi - lo) * 9 / 10;
    *klo = lo; *khi = hi > lo ? hi : lo + 1;
}
static int knob2raw(int k, int v) { int lo, hi; krange(k, &lo, &hi); return lo + (int)lround((double)v * (hi - lo) / 127.0); }
static int raw2knob(int k, int r) { int lo, hi; krange(k, &lo, &hi); int v = (int)lround((double)(r - lo) * 127.0 / (hi - lo)); return v < 0 ? 0 : v > 127 ? 127 : v; }
static void env_apply(void)
{
    for (int k = 0; k < NE; k++) {
        int i = C.pidx[k]; if (i < 0) continue;
        int v = *env_field(k) >= 0 ? knob2raw(k, *env_field(k)) : env_base[k];
        int lo = C.E->param_min(C.eng, i), hi = C.E->param_max(C.eng, i);
        C.E->param_set(C.eng, i, v < lo ? lo : v > hi ? hi : v);
    }
}
static int env_value(int k) { return C.pidx[k] >= 0 ? C.E->param_get(C.eng, C.pidx[k]) : 0; }   /* raw */
static int env_knob(int k) { return C.pidx[k] >= 0 ? raw2knob(k, env_value(k)) : 0; }
static double text_seconds(const char *t)
{
    double x = atof(t);
    if (strstr(t, "ms")) return x / 1000.0;
    if (strchr(t, 's')) return x;
    return x;
}

/* ------------------------------------------------------------------ the sound */
static void sound_load(void)
{
    int i = P()->sound; if (i < 0 || i >= C.npre) i = 0;
    static int loaded = -1;
    if (i == loaded && C.snd_eng[i] == C.ce) { env_apply(); return; }   /* same sound (pattern change): only the knob settings */
    loaded = i;
    if (C.snd_eng[i] != C.ce) engine_switch(C.snd_eng[i]);
    /* a fresh start for the new sound: voices still in a (silent) release would keep the synth from loading the
       sound at once, and the first chord would glide from the old sound's settings (measured: up to +5 dB) */
    all_off(0); C.E->reset(C.eng);
    C.E->preset_load(C.eng, C.pre[i]);
    if (C.lvidx >= 0) C.E->param_set(C.eng, C.lvidx, (int)(C.E->param_get(C.eng, C.lvidx) * 0.316f + 0.5f));   /* (level x (v/100)^2: -20 dB, so the synth's own peak limiter never engages: it releases slowly and made every chord after a loud one quieter; snd_levels.h makes it up) */
    for (int k = 0; k < NE; k++) env_base[k] = C.pidx[k] >= 0 ? C.E->param_get(C.eng, C.pidx[k]) : 0;
    env_apply();
    {   /* let the synth's smoothed LEVEL settle on the value just set (silent: no voices yet); otherwise the first
           chord's attack plays through a 10 ms glide from the full level (+5..+10 dB on plucks) */
        static float zl[256], zr[256];
        for (int k = 0; k < 12; k++) C.E->render(C.eng, zl, zr, 256);
    }
}

/* ------------------------------------------------------------------ the current chord */
static void block_stop(int frame) { for (int i = 0; i < C.block_n; i++) out_off(frame, C.block_note[i]); C.block_n = 0; }
static int cur_style(void) { return C.cur_live || C.seq_style < 0 ? P()->style : C.seq_style; }
static int cur_var(void) { return C.cur_live || C.seq_style < 0 ? P()->var : C.seq_var; }
static void block_start(int frame, int vel)
{
    block_stop(frame);
    for (int i = 0; i < C.cur_n; i++) {
        int n = C.cur_note[i] + 12 * C.oct;
        out_on(frame, n, vel);                             /* (all at once: a scheduled roll could outlive a quick release) */
        C.block_note[C.block_n++] = n;
    }
}
/* what sounds changes (the keys, or the sequencer): n == 0 = silence */
static void chord_set(int frame, const int *notes, int n, const char *label, int live, int vel, int retrig)
{
    int same = n == C.cur_n && live == C.cur_live;
    for (int i = 0; same && i < n; i++) if (notes[i] != C.cur_note[i]) same = 0;
    if (same && !retrig) return;
    int was = C.cur_n;
    C.cur_n = n; C.cur_live = live;
    for (int i = 0; i < n; i++) C.cur_note[i] = notes[i];
    snprintf(C.cur_label, sizeof C.cur_label, "%s", n ? label : "");
    if (cur_style() == 0) {
        if (n) block_start(frame, vel); else block_stop(frame);
    } else {
        block_stop(frame);
        if (n && (!was || !CK.playing)) { if (!CK.playing && !was) C.ph_restart = 1; }
    }
}
static void live_chord_from_keys(void)
{
    C.live_n = 0; C.live_label[0] = 0;
    if (!C.nheld) return;
    if (C.chordmode) {
        int k = C.held[C.nheld - 1];
        int pc = ((k - P()->key) % 12 + 12) % 12;
        int v[4]; char l[24];
        C.live_n = cs_voice(P()->set, pc, P()->key, P()->center, v, l, sizeof l);
        memcpy(C.live_note, v, sizeof v); snprintf(C.live_label, sizeof C.live_label, "%s", l);
    } else {
        int s[16], n = 0;
        for (int i = C.nheld - 1; i >= 0 && n < 4; i--) s[n++] = C.held[i];   /* the newest four */
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (s[j] < s[i]) { int t = s[i]; s[i] = s[j]; s[j] = t; }
        C.live_n = n; memcpy(C.live_note, s, sizeof(int) * n);
        cs_name_notes(s, n, C.live_label, sizeof C.live_label);
    }
}
static void refresh(int frame, int retrig)
{
    if (C.live_n) chord_set(frame, C.live_note, C.live_n, C.live_label, 1, C.vel_live, retrig);
    else if (CK.playing && C.seq_n) chord_set(frame, C.seq_note, C.seq_n, C.seq_label, 0, 100, retrig);
    else chord_set(frame, NULL, 0, "", 0, 0, 0);
}

/* ------------------------------------------------------------------ recording */
static void step_write(int s, const int *notes, int n, const char *label)
{
    if (s < 0 || s >= NSTEP) return;
    step_t *t = &P()->st[s];
    memset(t, 0, sizeof *t);
    t->n = (signed char)n; t->style = -1;
    for (int i = 0; i < n && i < 4; i++) t->note[i] = (unsigned char)notes[i];
    snprintf(t->label, sizeof t->label, "%s", label);
    P()->used = 1;
    cm_dirty();
}
static void step_tie(int s) { if (s >= 0 && s < NSTEP) { memset(&P()->st[s], 0, sizeof(step_t)); P()->st[s].tie = 1; P()->st[s].style = -1; cm_dirty(); } }

/* ------------------------------------------------------------------ keys */
static void ch_key(part_t *p, int frame, int note, int vel)
{
    (void)p;
    if (vel > 0) {
        for (int i = 0; i < C.nheld; i++) if (C.held[i] == note) { memmove(C.held + i, C.held + i + 1, sizeof(int) * (size_t)(C.nheld - i - 1)); C.nheld--; break; }
        if (C.nheld < 16) C.held[C.nheld++] = note;
        C.vel_live = vel;
        live_chord_from_keys();
        if (C.edit && !CK.playing) {                       /* step edit: write, next step */
            step_write(C.sel, C.live_note, C.live_n, C.live_label);
            C.sel = (C.sel + 1) % (P()->last > 0 ? P()->last : 1);
        } else if (C.rec && CK.playing) {
            long into = (CK.ptick - C.seq0) % STEPLEN[P()->steplen];
            int s = into > STEPLEN[P()->steplen] * 3 / 4 ? (C.step + 1) % P()->last : C.step;
            step_write(s, C.live_note, C.live_n, C.live_label);
        }
        refresh(frame, 1);
    } else {
        int was = C.nheld;
        for (int i = 0; i < C.nheld; i++) if (C.held[i] == note) { memmove(C.held + i, C.held + i + 1, sizeof(int) * (size_t)(C.nheld - i - 1)); C.nheld--; break; }
        if (was == C.nheld) return;
        if ((C.sustain || C.hold) && !C.nheld) return;     /* (the pedal / HOLD keeps the chord) */
        live_chord_from_keys();
        refresh(frame, C.nheld > 0);
    }
}
static void ch_cc(part_t *p, int frame, int cc, int val)
{
    (void)p;
    if (cc == 64) {
        C.sustain = val >= 64;
        if (!C.sustain && !C.hold && !C.nheld) { live_chord_from_keys(); refresh(frame, 0); }
        return;
    }
    if (cc == -1) { if (C.local) synth_midi(frame, 0xE0, val & 127, (val >> 7) & 127); return; }
    if (cc == 1 && C.local) synth_midi(frame, 0xB0, 1, val);
}

/* ------------------------------------------------------------------ the phrase */
static void phrase_tick(int frame, long tick)
{
    if (!C.cur_n) return;
    int style = cur_style();
    const char *txt = ph_text(style, cur_var());
    if (!txt) return;
    if (C.ph_restart) { C.ph0 = tick; C.ph_restart = 0; }
    long rel = tick - (CK.playing ? C.seq0 : C.ph0);
    if (rel < 0) return;
    long k = rel / 24; int off = (int)(rel % 24);
    int sw = P()->swing * 12 / 100;                        /* swing: odd sixteenths late, up to a triplet feel */
    if (off != ((k & 1) ? sw : 0)) return;
    int len = (int)strlen(txt);
    int pos = (int)(k % len);
    char c = txt[pos];
    if (c == '-' || c == '.') return;
    int dur = 1; while (dur < len && txt[(pos + dur) % len] == '-') dur++;
    double gt = P()->gate / 100.0;
    if (P()->grnd) {                                       /* GATE RND: each note's length varies around GATE */
        C.rnd = C.rnd * 1103515245u + 12345u;
        double u = ((C.rnd >> 8) & 0xFFFF) / 65536.0 * 2 - 1;
        gt *= 1 + u * 0.85 * P()->grnd / 100.0;
        if (gt < 0.08) gt = 0.08;
    }
    double ds = dur * 24 * CK.spt * gt;
    if (ds < 0.02 * SR) ds = 0.02 * SR;
    int acc = (c >= 'A' && c <= 'Z') || (pos % 4 == 0);
    int vel = acc ? 112 : 92;
    if (C.cur_live) vel = vel * (60 + C.vel_live) / 187;
    int n4[8], nn = 0;                                     /* the notes this symbol plays */
    int *cn = C.cur_note, cnum = C.cur_n;
    switch (c) {
    case 'x': case 'X': case 'd': case 'D': for (int i = 0; i < cnum; i++) n4[nn++] = cn[i]; break;
    case 'u': case 'U': for (int i = cnum - 1; i >= 0; i--) n4[nn++] = cn[i]; break;
    case 'c': case 'C': for (int i = cnum > 3 ? 1 : 0; i < cnum; i++) n4[nn++] = cn[i]; break;
    case 'r': { C.rnd = C.rnd * 1103515245u + 12345u; int lo = cnum > 3 ? 1 : 0; n4[nn++] = cn[lo + (int)((C.rnd >> 16) % (unsigned)(cnum - lo))]; break; }
    default:
        if (c >= '1' && c <= '8') {
            int i = (c - '1') % 4, o = c >= '5' ? 12 : 0;
            if (i >= cnum) i = cnum - 1;
            n4[nn++] = cn[i] + o;
        }
    }
    int strum = (c == 'd' || c == 'D' || c == 'u' || c == 'U') ? C.strum_ms : 0;
    for (int i = 0; i < nn; i++) {
        int n = n4[i] + 12 * C.oct;
        long long at = CK.now + frame + (long long)(i * strum * 0.001 * SR);
        if (at == CK.now + frame) out_on(frame, n, vel); else cm_sched(at, &PART[0], 1, n, vel);
        cm_sched(at + (long long)ds, &PART[0], 0, n, 0);
    }
}

/* ------------------------------------------------------------------ the sequencer */
static void seq_apply_step(int frame)
{
    step_t *t = &P()->st[C.step];
    if (t->tie) { if (cur_style() == 0 && !C.cur_live) { /* held on */ } }
    else if (t->n > 0) {
        C.seq_n = t->n; for (int i = 0; i < t->n; i++) C.seq_note[i] = t->note[i];
        snprintf(C.seq_label, sizeof C.seq_label, "%s", t->label);
        C.seq_style = t->style; C.seq_var = t->var;
        if (!C.live_n) chord_set(frame, C.seq_note, C.seq_n, C.seq_label, 0, 100, 1);
    } else {
        C.seq_n = 0;
        if (!C.live_n) chord_set(frame, NULL, 0, "", 0, 0, 0);
    }
}
static void pattern_switch(int i, int frame)
{
    if (i < 0 || i >= NPAT) return;
    int old_sound = P()->sound;
    C.cur = i;
    if (P()->last < 1) P()->last = 16;
    cm_set_bpm(P()->bpm10 / 10.0);
    if (P()->sound != old_sound || 1) sound_load();
    if (C.sel >= P()->last) C.sel = 0;
    live_chord_from_keys();
    if (C.live_n) refresh(frame, 1);                       /* (a held chord follows the new pattern's set at once) */
    cm_dirty();
}
static void ch_tick(part_t *p, int frame, long tick)
{
    (void)p;
    if (CK.playing) {
        long rel = CK.ptick - C.seq0;
        int L = STEPLEN[P()->steplen];
        if (rel % L == 0) {
            long s = rel / L;
            int at_end = C.step + 1 >= P()->last || s == 0;
            int at_bar = (rel % (4 * PPQN)) == 0;
            if (C.next >= 0 && s > 0 && (C.pchange == 1 || (C.pchange == 2 && at_bar) || (C.pchange == 0 && at_end))) {
                pattern_switch(C.next, frame); C.next = -1;
                C.seq0 = CK.ptick; C.step = 0; rel = 0; L = STEPLEN[P()->steplen];
            } else if (s > 0) {
                C.step = (C.step + 1) % (P()->last > 0 ? P()->last : 1);
                if (C.step == 0 && C.gen) gen_mutate();
            }
            else C.step = 0;
            seq_apply_step(frame);
        }
    }
    phrase_tick(frame, CK.playing ? CK.ptick : tick);
}
static void ch_transport(part_t *p, int frame, int playing)
{
    (void)p;
    if (playing) {
        if (C.next >= 0) { pattern_switch(C.next, frame); C.next = -1; }
        C.seq0 = 0; C.step = 0;
    } else {
        all_off(frame);
        C.seq_n = 0; C.cur_n = 0; C.cur_label[0] = 0;
        if (C.live_n) refresh(frame, 1);
    }
}
static void ch_event(part_t *p, int frame, int a, int b, int c) { (void)p; if (a == 1) out_on(frame, b, c); else out_off(frame, b); }
/* EQ (low shelf 200 Hz, peak 1 kHz, high shelf 5 kHz, +-12 dB; 64 = flat) and PAN (equal power), on the chord sound */
typedef struct { double b0, b1, b2, a1, a2; double z[2][2]; } bq_t;
static bq_t BQ[3]; static int bq_set[3] = {-1, -1, -1};
static void bq_make(bq_t *q, int type, double f, double gdb)
{
    double A = pow(10.0, gdb / 40.0), w = 2 * M_PI * f / SR, cw = cos(w), sw = sin(w), S = 1.0, Q = 0.8, al;
    double b0, b1, b2, a0, a1, a2;
    if (type == 1) { al = sw / (2 * Q); b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; }
    else {
        al = sw / 2 * sqrt((A + 1 / A) * (1 / S - 1) + 2); double sa = 2 * sqrt(A) * al;
        if (type == 0) { b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
                         a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa; }
        else { b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
               a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa; }
    }
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0; q->a1 = a1 / a0; q->a2 = a2 / a0;
}
static float pan_g[2] = {1, 1};
static void eq_pan(float *l, float *r, int n)
{
    const int g[3] = {P()->eql, P()->eqm, P()->eqh}; static const double F[3] = {200, 1000, 5000};
    for (int b = 0; b < 3; b++) {
        if (g[b] != bq_set[b]) { bq_make(&BQ[b], b, F[b], (g[b] - 64) * 12.0 / 63.0); bq_set[b] = g[b]; }
        if (g[b] == 64) continue;
        bq_t *q = &BQ[b];
        for (int c = 0; c < 2; c++) {
            float *x = c ? r : l; double z0 = q->z[c][0], z1 = q->z[c][1];
            for (int i = 0; i < n; i++) { double in = x[i], y = q->b0 * in + z0; z0 = q->b1 * in - q->a1 * y + z1; z1 = q->b2 * in - q->a2 * y; x[i] = (float)y; }
            if (fabs(z0) < 1e-20) z0 = 0; if (fabs(z1) < 1e-20) z1 = 0;
            q->z[c][0] = z0; q->z[c][1] = z1;
        }
    }
    double pp = (P()->pan - 64) / 63.0; if (pp > 1) pp = 1; if (pp < -1) pp = -1;
    float tl = pp > 0 ? (float)cos(pp * M_PI / 2) : 1.f, tr = pp < 0 ? (float)cos(-pp * M_PI / 2) : 1.f;   /* (balance: centre = untouched) */
    for (int i = 0; i < n; i++) { pan_g[0] += (tl - pan_g[0]) * 0.002f; pan_g[1] += (tr - pan_g[1]) * 0.002f; l[i] *= pan_g[0]; r[i] *= pan_g[1]; }
}
static void ch_render(part_t *p, float *L, float *R, float *sd, float *sr, int n)
{
    float l[MAXF], r[MAXF];
    int done = 0;
    if (C.engine == 1 && cm_instr) done = cm_instr(C.ev, C.nev, l, r, n);   /* the AU instrument (shell) */
    C.nev = 0;
    if (!done) { if (C.engine == 0) { C.E->render(C.eng, l, r, n); float sg = C.snd_gain[P()->sound < C.npre ? P()->sound : 0]; if (sg != 1.f) for (int i = 0; i < n; i++) { l[i] *= sg; r[i] *= sg; } } else { memset(l, 0, sizeof(float) * (size_t)n); memset(r, 0, sizeof(float) * (size_t)n); } }
    if (cm_insert) cm_insert(l, r, n);
    eq_pan(l, r, n);                     /* the AU insert (shell), before volume and sends */
    float g = P()->vol / 100.f; g *= g;
    float dg = P()->dly / 127.f, rg = P()->rev / 127.f; dg *= dg; rg *= rg;
    float pk = 0;
    for (int i = 0; i < n; i++) {
        float a = l[i] * g, b = r[i] * g;
        L[i] += a; R[i] += b;
        float m = 0.5f * (a + b);
        sd[i] += m * dg; sr[i] += m * rg;
        float x = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b); if (x > pk) pk = x;
    }
    p->peak = pk > p->peak ? pk : p->peak * 0.92f;
}


/* ------------------------------------------------------------------ RANDOM and GENERATIVE
   A progression is a walk over the 12 keys (bass notes) of the chord set. Each move is weighted by how strong the bass
   motion is (up a fourth / down a fifth strongest, then down a third, steps, up a fifth ...) times how central the
   key is to the set (its tonic, IV, V, vi first; borrowed chords less; chromatic rarely). Several candidates are
   drawn and the one that flows best, including the turn back to its first chord, is kept. GENERATIVE re-writes one
   or two chords (and now and then the phrase variation) each time the pattern comes round, so it keeps evolving. */
static unsigned gen_rnd(void) { C.rnd = C.rnd * 1103515245u + 12345u; return C.rnd >> 8; }
static double gen_u(void) { return (gen_rnd() & 0xFFFF) / 65536.0; }
static int set_is_minor(int set)
{
    const char *s = cs_symbol(set, 0);
    if (!s[0]) return 0;
    const char *q = s + 1; if (*q == '#' || *q == 'b') q++;
    return q[0] == 'm' && q[1] != 'a';                   /* ("m", "m7", "m9" ... not "M7") */
}
static double key_w(int set, int pc)
{
    static const double MAJ[12] = {1.0, .2, .7, .45, .7, 1.0, .25, 1.0, .45, .9, .5, .6};
    static const double MIN[12] = {1.0, .35, .5, .9, .4, 1.0, .25, .9, 1.0, .3, .9, .45};
    return (set_is_minor(set) ? MIN : MAJ)[pc];
}
static double move_w(int from, int to)
{
    static const double M[12] = {.05, .35, 1.3, 1.0, 1.0, 3.0, .15, 2.0, 1.4, 2.0, 1.4, .35};
    return M[((to - from) % 12 + 12) % 12];
}
static int gen_pick(int set, int from, int avoid)
{
    double w[12], tot = 0;
    for (int k = 0; k < 12; k++) { w[k] = (k == avoid ? 0.1 : 1.0) * key_w(set, k) * (from < 0 ? (k == 0 ? 4 : 1) : move_w(from, k)); tot += w[k]; }
    double r = gen_u() * tot;
    for (int k = 0; k < 12; k++) { r -= w[k]; if (r <= 0) return k; }
    return 0;
}
static double prog_score(int set, const int *k, int n)
{
    double sc = 0;
    for (int i = 0; i < n; i++) { int nx = k[(i + 1) % n]; sc += log(move_w(k[i], nx) * key_w(set, nx) + 1e-3); if (k[i] == nx) sc -= 2; }
    int seen[12] = {0}; for (int i = 0; i < n; i++) { if (seen[k[i]]++) sc -= 1.6 * seen[k[i]]; }   /* (variety: few repeats) */
    return sc + (k[0] == 0 ? 1.5 : 0);
}
static void gen_write(pat_t *t, const int *keys, const int *beats, int n)
{
    for (int s = 0; s < NSTEP; s++) { memset(&t->st[s], 0, sizeof(step_t)); t->st[s].style = -1; }
    int s = 0;
    for (int i = 0; i < n && s < NSTEP; i++) {
        int v[4]; char l[24];
        int cnt = cs_voice(t->set, ((keys[i] - t->key) % 12 + 12) % 12, t->key, t->center, v, l, sizeof l);
        step_t *x = &t->st[s];
        x->n = (signed char)cnt; for (int j = 0; j < cnt; j++) x->note[j] = (unsigned char)v[j];
        snprintf(x->label, sizeof x->label, "%s", l);
        s++;
        for (int b = 1; b < beats[i] && s < NSTEP; b++) t->st[s++].tie = 1;
    }
    t->last = s; t->used = 1;
}
static void gen_random(void)
{
    pat_t *t = P();
    static const int R[][9] = {     /* chord lengths in steps (count first) */
        {4, 4, 4, 4, 4}, {8, 4, 4, 4, 4, 4, 4, 4, 4}, {4, 8, 8, 8, 8}, {5, 4, 4, 4, 2, 2},
        {6, 2, 2, 4, 2, 2, 4}, {8, 2, 2, 2, 2, 2, 2, 2, 2}, {5, 4, 2, 2, 4, 4}, {6, 4, 4, 4, 4, 8, 8},
    };
    const int *r = R[gen_rnd() % (sizeof R / sizeof R[0])];
    int n = r[0], best[8], cand[8]; double bs = -1e9;
    for (int tries = 0; tries < 24; tries++) {
        int prev = -1;
        for (int i = 0; i < n; i++) { cand[i] = gen_pick(t->set, prev, i ? cand[i - 1] : -1); prev = cand[i]; }
        cand[0] = cand[0] % 12;
        double sc = prog_score(t->set, cand, n) + gen_u() * 1.5;   /* (a little chance: not always the same answer) */
        if (sc > bs) { bs = sc; memcpy(best, cand, sizeof best); }
    }
    int keys[8]; for (int i = 0; i < n; i++) keys[i] = (best[i] + t->key + 120) % 12;
    gen_write(t, keys, r + 1, n);
    /* a phrase to go with it (sometimes just held chords) */
    if (gen_u() < 0.12) t->style = 0; else { t->style = 1 + (int)(gen_rnd() % (unsigned)(ph_nstyles() - 1)); t->var = (int)(gen_rnd() % 8); }
    snprintf(t->name, sizeof t->name, "Random %d", (int)(gen_rnd() % 900) + 100);
}
static void gen_mutate(void)
{
    pat_t *t = P();
    int idx[NSTEP], n = 0;
    for (int s = 0; s < t->last; s++) if (t->st[s].n > 0) idx[n++] = s;
    if (n < 2) { gen_random(); return; }
    int changes = gen_u() < 0.3 ? 2 : 1;
    for (int c = 0; c < changes; c++) {
        int i = 1 + (int)(gen_rnd() % (unsigned)(n - 1));          /* (the first chord stays: the home) */
        int s = idx[i];
        int prevpc = ((t->st[idx[i - 1]].note[0] - t->key) % 12 + 12) % 12;
        int curpc = ((t->st[s].note[0] - t->key) % 12 + 12) % 12;
        int pc = gen_pick(t->set, prevpc, curpc);
        int v[4]; char l[24];
        int cnt = cs_voice(t->set, pc, t->key, t->center, v, l, sizeof l);
        step_t *x = &t->st[s];
        x->n = (signed char)cnt; for (int j = 0; j < cnt; j++) x->note[j] = (unsigned char)v[j];
        snprintf(x->label, sizeof x->label, "%s", l);
    }
    if (t->style && gen_u() < 0.25) t->var = (int)(gen_rnd() % 8);
    t->used = 1;
    cm_dirty();
}

/* ------------------------------------------------------------------ patterns: factory */
static void pat_default(pat_t *t, int i)
{
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%d-%d", i / 8 + 1, i % 8 + 1);
    t->last = 16; t->steplen = 3; t->bpm10 = 1200; t->set = 0; t->key = 0; t->style = 0; t->var = 0; t->sound = 0;
    t->cut = t->res = t->sub = -1; t->ea = t->ed = t->es = t->er = t->eg = -1; t->eql = t->eqm = t->eqh = t->pan = 64; t->gate = 80; t->swing = 0; t->dly = 20; t->rev = 40; t->vol = 80; t->center = 66;
    for (int s = 0; s < NSTEP; s++) t->st[s].style = -1;
}
/* a factory pattern: a progression of keys (bass notes, -1 = tie) on one set */
static void pat_factory(int i, const char *name, int set, int key, int bpm, int style, int var, int sound, int steplen, const char *keys)
{
    pat_t *t = &C.pat[i];
    pat_default(t, i);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->set = set; t->key = key; t->bpm10 = bpm * 10; t->style = style; t->var = var; t->sound = sound; t->steplen = steplen;
    int s = 0;
    for (const char *k = keys; *k && s < NSTEP; k++) {
        if (*k == ' ') continue;
        if (*k == '-') { t->st[s].tie = 1; s++; continue; }
        if (*k == '.') { s++; continue; }
        static const char *H = "C#D#EF#G#A#B";   /* C c D d E F f G g A a B: lower case = sharp */
        static const char *K = "CcDdEFfGgAaB";
        const char *f = strchr(K, *k); (void)H;
        if (!f) continue;
        int pc = (int)(f - K), v[4]; char l[24];
        int n = cs_voice(set, pc, key, 66, v, l, sizeof l);
        memcpy(&t->st[s], &(step_t){0}, sizeof(step_t));
        t->st[s].n = (signed char)n; t->st[s].style = -1;
        for (int j = 0; j < n; j++) t->st[s].note[j] = (unsigned char)v[j];
        snprintf(t->st[s].label, sizeof t->st[s].label, "%s", l);
        s++;
    }
    t->last = s > 0 ? s : 16;
    t->used = 1;
}
int cm_factory_misses;
static int find_set(const char *name) { for (int i = 0; i < cs_count(); i++) if (!strcmp(cs_name(i), name)) return i; cm_factory_misses++; return 0; }
static int find_sound(const char *name)
{
    static const char *const OLD[][2] = {{"AURA/Juno Poly Pad", "AURA/Poly Chord Pad"}, {"Juno Poly Pad", "Poly Chord Pad"},
                                         {"AURA/Ensemble 106", "AURA/Classic Ensemble"}, {"Ensemble 106", "Classic Ensemble"}};   /* (renamed: saved patterns still find them) */
    for (int i = 0; i < 4; i++) if (!strcmp(name, OLD[i][0])) { name = OLD[i][1]; break; }
    const char *slash = strchr(name, '/'); char tag[16] = "";
    if (slash && slash - name < 15) { memcpy(tag, name, (size_t)(slash - name)); tag[slash - name] = 0; name = slash + 1; }
    for (int i = 0; i < C.npre; i++) {
        int e = C.snd_eng[i];
        if (tag[0] && strcasecmp(tag, ENG_TAG[e])) continue;
        if (!strcmp(ENG[e]->preset_name(C.inst[e], C.pre[i]), name)) return i;
    }
    cm_factory_misses++;
    return 0;
}
static void factory(void)
{
    for (int i = 0; i < NPAT; i++) pat_default(&C.pat[i], i);
    /* steplen 3 = a quarter per step; the keys name the bass notes (lower case = sharp) */
    pat_factory(0, "Pop Drive", find_set("Pop Bright"), 0, 110, 1, 0, find_sound("Aura Pluck"), 3, "C---G---A---F---");
    pat_factory(1, "Ballad", find_set("Pop Ballad"), 0, 76, 8, 1, find_sound("Velvet EP"), 3, "C---B---A---G---F---E---D---G---");
    pat_factory(2, "Neo Soul", find_set("Neo Soul 1"), 0, 84, 6, 4, find_sound("Velvet EP"), 3, "D---G---C---A---");
    pat_factory(3, "Deep House", find_set("Deep House"), 0, 122, 5, 0, find_sound("Organ 60"), 3, "C-------A-------F-------G-------");
    pat_factory(4, "Psy Roll", find_set("Psy Minor"), 0, 142, 3, 0, find_sound("Sync Pluck"), 3, "A-------F-------G-------E-------");
    pat_factory(5, "Trance Gate", find_set("Trance"), 0, 138, 5, 5, find_sound("Chord Stab"), 3, "C-------g-------a-------G-------");
    pat_factory(6, "Synthwave", find_set("Synthwave"), 0, 100, 2, 0, find_sound("Stab 1984"), 3, "A-------F-------C-------G-------");
    pat_factory(7, "Ambient Bloom", find_set("Healing"), 0, 70, 0, 0, find_sound("Dream Pad"), 3, "C-------d-------F-------A-------");
    pat_factory(8, "Jazz ii-V", find_set("Jazz ii-V"), 0, 96, 4, 2, find_sound("Aura Piano"), 3, "D-G-C---D-G-C-A-");
    pat_factory(9, "Bossa", find_set("Bossa"), 0, 128, 5, 4, find_sound("Velvet EP"), 3, "C---A---D---G---");
    pat_factory(10, "Lofi", find_set("Lofi"), 0, 78, 8, 2, find_sound("Velvet EP"), 3, "F---E---D---C---");
    pat_factory(11, "Gospel", find_set("Gospel"), 0, 72, 6, 7, find_sound("Organ 60"), 3, "C---F---C---G---");
    pat_factory(12, "Cinematic", find_set("Cinematic Epic"), 0, 90, 4, 0, find_sound("Aura Strings"), 3, "C-------g-------a-------G-------");
    pat_factory(13, "Dub Techno", find_set("Dub Techno"), 0, 120, 5, 6, find_sound("Velvet Pad"), 3, "C-------C-------d-------C-------");
    pat_factory(14, "Funk", find_set("Funk"), 0, 104, 5, 2, find_sound("Aura Pluck"), 3, "C-------F-------C-------G-------");
    pat_factory(15, "Goa Night", find_set("Goa"), 0, 145, 3, 6, find_sound("Glass Pluck"), 3, "C-------D-------a-------C-------");
    /* banks 3-8 */
    static const struct { const char *name, *set; int bpm, style, var; const char *sound, *keys; } X[48] = {
        {"Pop Anthem", "Pop Anthem", 118, 4, 3, "Analog Ensemble", "C---G---A---F---"},
        {"Pop Uplift", "Pop Uplift", 124, 7, 1, "Aura Piano", "F---G---E---A---"},
        {"Sad Ballad", "Pop Sad", 70, 8, 1, "Aura Piano", "C-------F-------D-------G-------"},
        {"Pop Minor", "Pop Minor", 96, 2, 1, "Glass Pluck", "C---g---a---G---"},
        {"Synth Pop", "Synth Pop", 112, 4, 3, "Synth Brass", "C---a---F---G---"},
        {"City Pop", "City Pop", 104, 5, 2, "Velvet EP", "F---E---D---G---"},
        {"Rock Chords", "Rock Chords", 128, 7, 3, "Synth Brass", "C---a---F---G---"},
        {"Power Rock", "Rock", 140, 4, 3, "Neon Horns", "E---G---A---D---"},
        {"Jazz Ballad", "Jazz Ballad", 66, 6, 0, "Velvet EP", "D-------G-------C-------A-------"},
        {"Jazz Minor", "Jazz Minor", 92, 8, 2, "Aura Piano", "D---G---C-------"},
        {"Jazz Blues", "Jazz Blues", 110, 5, 4, "Organ 60", "C-------F-------C-------G---F---"},
        {"Modal", "Jazz Modal", 98, 1, 5, "Ice Chime", "D-------E-------F-------G-------"},
        {"Bossa Minor", "Bossa Minor", 124, 5, 4, "Velvet EP", "C-------D---G---"},
        {"Neo Soul 2", "Neo Soul 2", 80, 6, 3, "Velvet EP", "C---F---a---G---"},
        {"Neo Soul 3", "Neo Soul 3", 86, 8, 6, "Aura Piano", "C---A---D---G---"},
        {"Lofi Night", "Lofi Minor", 74, 6, 1, "Velvet EP", "C---g---F---G---"},
        {"Gospel R&B", "Gospel R&B", 68, 6, 7, "Organ 60", "C---E---F---G---"},
        {"Soul Strut", "Soul", 98, 5, 2, "Aura Piano", "C---A---D---G---"},
        {"Funk Minor", "Funk Minor", 100, 5, 2, "Aura Pluck", "C-------F-------"},
        {"Blues Shuffle", "Blues", 96, 8, 7, "Organ 60", "C---C---F---C---G---F---C---G---"},
        {"Reggae Skank", "Reggae", 76, 5, 1, "Organ 60", "C-------F-------G-------F-------"},
        {"Latin Minor", "Latin", 112, 7, 7, "Aura Piano", "C-------F-------G-------C-------"},
        {"Bossa Dream", "Bossa", 118, 8, 4, "Velvet EP", "D---G---C---A---"},
        {"Mixolydian", "Mixolydian", 108, 7, 0, "Synth Brass", "C-------a-------F-------C-------"},
        {"Jazz House", "Jazz House", 124, 5, 0, "Velvet EP", "D-------C-------F-------G-------"},
        {"Organ House", "Organ House", 124, 5, 6, "Organ 60", "C-------A-------F-------G-------"},
        {"Techno Stab", "Techno", 130, 5, 6, "Aura Pluck", "C-------d-------C-------a-------"},
        {"Dub Skank", "Dub Techno", 118, 5, 1, "Velvet Pad", "C-------F-------"},
        {"EDM Drop", "EDM", 128, 4, 4, "Synth Brass", "F---G---E---A---"},
        {"EDM Minor Arp", "EDM Minor", 126, 2, 0, "Sync Pluck", "C---g---a---G---"},
        {"Future Bass", "Future Bass", 150, 4, 4, "Analog Ensemble", "F---G---E---A---"},
        {"Deep Gate", "Deep House", 120, 5, 5, "Glass Pluck", "C-------G-------F-------a-------"},
        {"Trance Uplift", "Trance Uplift", 138, 2, 1, "Chord Stab", "F---G---A---C---"},
        {"Trance Pads", "Trance", 136, 4, 4, "Velvet Pad", "C-------g-------a-------F-------"},
        {"Psy Phrygian", "Psy Phrygian", 144, 3, 1, "Sync Pluck", "C-------d-------C-------a-------"},
        {"Psy Spiral", "Psy Minor", 145, 3, 4, "Glass Pluck", "A-------a-------A-------E-------"},
        {"Goa Waves", "Goa", 146, 3, 2, "Marimba Pluck", "E-------F-------D-------E-------"},
        {"Synthwave Drive", "Synthwave 2", 108, 1, 0, "Stab 1984", "C---g---a---G---"},
        {"Retro Night", "Synthwave", 96, 4, 2, "Poly Brass 84", "A---F---C---G---"},
        {"Arp Cascade", "Trance", 140, 2, 2, "Glass Pluck", "C---g---a---F---"},
        {"Ambient Drift", "Ambient", 64, 0, 0, "Ether Halo", "C-------F-------G-------A-------"},
        {"Lydian Sky", "Ambient Lydian", 70, 1, 1, "Ice Chime", "C-------D-------G-------F-------"},
        {"Healing Tide", "Healing", 60, 6, 0, "Poly Chord Pad", "C-------F-------A-------E-------"},
        {"Minor Mist", "Ambient Minor", 66, 0, 0, "Slow Bloom", "C-------a-------g-------G-------"},
        {"Cinematic Hope", "Cinematic Hope", 84, 4, 1, "Cinema Swell", "C-------F-------A-------G-------"},
        {"Dark Halls", "Cinematic Horror", 72, 0, 0, "Night Choir", "C-------d-------C-------g-------"},
        {"Baroque", "Baroque", 100, 8, 0, "Aura Piano", "C---A---D---G---"},
        {"Romantic", "Romantic", 72, 8, 5, "Aura Strings", "C---A---D---G---"},
    };
    for (int i = 0; i < 48; i++)
        pat_factory(16 + i, X[i].name, find_set(X[i].set), 0, X[i].bpm, X[i].style, X[i].var, find_sound(X[i].sound), 3, X[i].keys);

    /* even loudness: the plucky phrase patterns a little louder */
    static const int LOUD[][2] = {{0, 108}, {1, 106}, {2, 81}, {3, 81}, {4, 127}, {5, 81}, {6, 96}, {7, 80}, {8, 81}, {9, 84}, {10, 95}, {11, 80}, {12, 80}, {13, 116}, {14, 80}, {15, 97}, {16, 80}, {17, 80}, {18, 127}, {19, 96}, {20, 80}, {21, 80}, {22, 80}, {23, 68}, {24, 94}, {25, 90}, {26, 80}, {27, 97}, {28, 80}, {29, 81}, {30, 117}, {31, 84}, {32, 80}, {33, 83}, {34, 73}, {35, 81}, {36, 99}, {37, 81}, {38, 100}, {39, 80}, {40, 88}, {41, 80}, {42, 74}, {43, 127}, {44, 80}, {45, 127}, {46, 88}, {47, 80}, {48, 109}, {49, 115}, {50, 127}, {51, 97}, {52, 110}, {53, 103}, {54, 80}, {55, 98}, {56, 84}, {57, 108}, {58, 80}, {59, 80}, {60, 97}, {61, 80}, {62, 100}, {63, 127}};
    if (!getenv("CM_NOLOUD")) for (int i = 0; i < (int)(sizeof LOUD / sizeof LOUD[0]); i++) C.pat[LOUD[i][0]].vol = LOUD[i][1];
}

/* ------------------------------------------------------------------ init, state */
/* left out after the preset audit (tests/preset_audit.c): a notch "organ" whose FILTER barely changes anything,
   and a second "Poly Keys" (the list would show the same name twice) */
static int dropped(int e, const char *name)
{
    static const char *const DROP[] = {"FATHOM/Notch Organ", "FATHOM/Poly Keys", 0};
    char k[96]; snprintf(k, sizeof k, "%s/%s", ENG_TAG[e], name);
    for (int i = 0; DROP[i]; i++) if (!strcmp(DROP[i], k)) return 1;
    return 0;
}
static void ch_init(part_t *p)
{
    memset(&C, 0, sizeof C);
    p->s = &C;
    for (int e = 0; e < NENG; e++) C.inst[e] = ENG[e]->create(SR);
    /* the sound list, grouped by category (Pad, Strings, Brass, Keys, …), engines mixed inside each */
    for (int k = 0; KEEP_CAT[k]; k++)
        for (int e = 0; e < NENG; e++) {
            int np = ENG[e]->npresets(C.inst[e]);
            for (int i = 1; i < np && C.npre < 128; i++)
                if (!strcmp(ENG[e]->preset_cat(C.inst[e], i), KEEP_CAT[k]) && !dropped(e, ENG[e]->preset_name(C.inst[e], i))) { C.snd_eng[C.npre] = e; C.snd_gain[C.npre] = 1.f; C.pre[C.npre++] = i; }
        }
    snd_levels();
    C.ce = 0; C.E = ENG[0]; C.eng = C.inst[0];
    find_params();
    C.chordmode = 1; C.outch = 1; C.inch = 0; C.local = 1; C.strum_ms = 18; C.oct = 0; C.pchange = 0;
    C.next = -1; C.rnd = 0x1234567u;
    factory();
    C.cur = 0;
    sound_load();
    cm_set_bpm(P()->bpm10 / 10.0);
}

static void step_json(jb_t *b, const step_t *t)
{
    jb_ch(b, '['); jb_int(b, t->tie ? -1 : t->n);
    jb_ch(b, ','); jb_str_utf8(b, t->label);
    jb_ch(b, ','); jb_ch(b, '[');
    for (int i = 0; i < t->n; i++) { if (i) jb_ch(b, ','); jb_int(b, t->note[i]); }
    jb_ch(b, ']'); jb_ch(b, ','); jb_int(b, t->style); jb_ch(b, ','); jb_int(b, t->var); jb_ch(b, ']');
}
static void ch_state(part_t *p, jb_t *b)
{
    (void)p;
    pat_t *t = P();
    jb_key(b, "pat"); jb_int(b, C.cur);
    jb_key(b, "next"); jb_int(b, C.next);
    jb_key(b, "pats"); jb_ch(b, '[');
    for (int i = 0; i < NPAT; i++) { if (i) jb_ch(b, ','); jb_ch(b, '['); jb_str_utf8(b, C.pat[i].name); jb_ch(b, ','); jb_int(b, C.pat[i].used); jb_ch(b, ']'); }
    jb_ch(b, ']');
    jb_key(b, "p"); jb_ch(b, '{');
    jb_key(b, "name"); jb_str_utf8(b, t->name);
#define F(k) jb_key(b, #k); jb_int(b, t->k);
    F(last) F(steplen) F(bpm10) F(set) F(key) F(style) F(var) F(sound) F(gate) F(swing) F(dly) F(rev) F(vol) F(center) F(grnd) F(eql) F(eqm) F(eqh) F(pan)
#undef F
    {   /* the envelope as it is now (the sound's or set), with AURA's texts and times for the drawing */
        static const char *const K[NE] = {"ea", "ed", "es", "er", "eg", "cut", "res", "sub"};
        for (int k = 0; k < NE; k++) { jb_key(b, K[k]); jb_int(b, env_knob(k)); }
        jb_key(b, "eset"); jb_ch(b, '['); for (int k = 0; k < NE; k++) { if (k) jb_ch(b, ','); jb_int(b, *env_field(k) >= 0); } jb_ch(b, ']');
        jb_key(b, "ehas"); jb_ch(b, '['); for (int k = 0; k < NE; k++) { if (k) jb_ch(b, ','); jb_int(b, C.pidx[k] >= 0); } jb_ch(b, ']');
        jb_key(b, "etext"); jb_ch(b, '[');
        for (int k = 0; k < NE; k++) { if (k) jb_ch(b, ','); jb_str_utf8(b, C.pidx[k] >= 0 ? C.E->param_text(C.eng, C.pidx[k], env_value(k)) : ""); }
        jb_ch(b, ']');
        jb_key(b, "esec"); jb_ch(b, '[');
        for (int k = 0; k < 4; k++) { if (k) jb_ch(b, ','); jb_num(b, k == 2 ? env_value(2) / 127.0 : text_seconds(C.pidx[k] >= 0 ? C.E->param_text(C.eng, C.pidx[k], env_value(k)) : "0")); }
        jb_ch(b, ']');
    }
    jb_ch(b, '}');
    jb_key(b, "steps"); jb_ch(b, '[');
    for (int i = 0; i < NSTEP; i++) { if (i) jb_ch(b, ','); step_json(b, &t->st[i]); }
    jb_ch(b, ']');
    jb_key(b, "sel"); jb_int(b, C.sel);
    jb_key(b, "edit"); jb_int(b, C.edit);
    jb_key(b, "rec"); jb_int(b, C.rec);
    jb_key(b, "gen"); jb_int(b, C.gen);
    jb_key(b, "hold"); jb_int(b, C.hold);
    jb_key(b, "chordmode"); jb_int(b, C.chordmode);
    jb_key(b, "outch"); jb_int(b, C.outch);
    jb_key(b, "inch"); jb_int(b, C.inch);
    jb_key(b, "outport"); jb_str_utf8(b, C.outport);
    jb_key(b, "local"); jb_int(b, C.local);
    jb_key(b, "engine"); jb_int(b, C.engine);
    jb_key(b, "strum"); jb_int(b, C.strum_ms);
    jb_key(b, "oct"); jb_int(b, C.oct);
    jb_key(b, "pchange"); jb_int(b, C.pchange);
    /* the lists */
    jb_key(b, "sets"); jb_ch(b, '[');
    for (int i = 0; i < cs_count(); i++) { if (i) jb_ch(b, ','); jb_str_utf8(b, cs_name(i)); }
    jb_ch(b, ']');
    jb_key(b, "pads"); jb_ch(b, '[');                     /* the 12 chords of the set in this key */
    for (int k = 0; k < 12; k++) {
        int v[4]; char l[24];
        int n = cs_voice(t->set, k, t->key, t->center, v, l, sizeof l);
        if (k) jb_ch(b, ',');
        jb_ch(b, '['); jb_str_utf8(b, l); jb_ch(b, ',');
        jb_ch(b, '['); for (int i = 0; i < n; i++) { if (i) jb_ch(b, ','); jb_int(b, v[i]); } jb_ch(b, ']'); jb_ch(b, ']');
    }
    jb_ch(b, ']');
    jb_key(b, "sounds"); jb_ch(b, '[');
    for (int i = 0; i < C.npre; i++) {
        if (i) jb_ch(b, ',');
        int e = C.snd_eng[i]; /* [name, category, engine tag]: the page shows name + category only (the engines stay behind the scenes) */
        jb_ch(b, '['); jb_str_utf8(b, ENG[e]->preset_name(C.inst[e], C.pre[i])); jb_ch(b, ','); jb_str_utf8(b, ENG[e]->preset_cat(C.inst[e], C.pre[i])); jb_ch(b, ','); jb_str_utf8(b, ENG_TAG[e]); jb_ch(b, ']');
    }
    jb_ch(b, ']');
    jb_key(b, "styles"); jb_ch(b, '[');
    for (int s = 0; s < ph_nstyles(); s++) {
        if (s) jb_ch(b, ',');
        jb_ch(b, '['); jb_str_utf8(b, ph_style(s));
        for (int v = 0; v < 8 && s > 0; v++) { jb_ch(b, ','); jb_str_utf8(b, ph_var_name(s, v)); }
        jb_ch(b, ']');
    }
    jb_ch(b, ']');
    jb_key(b, "steplens"); jb_ch(b, '[');
    for (int i = 0; i < NSTEPLEN; i++) { if (i) jb_ch(b, ','); jb_str_utf8(b, STEPNAME[i]); }
    jb_ch(b, ']');
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ch_set(part_t *p, const char *k, const js_t *m)
{
    (void)p;
    pat_t *t = P();
    int v = (int)js_num(m, "v", 0);
    if (!strcmp(k, "set")) {
        const char *f = js_strv(m, "f");
        if (!f) return 0;
        int changed = 1;
        if (!strcmp(f, "set")) t->set = clampi(v, 0, cs_count() - 1);
        else if (!strcmp(f, "key")) t->key = clampi(v, -12, 12);
        else if (!strcmp(f, "style")) { t->style = clampi(v, 0, ph_nstyles() - 1); all_off(0); refresh(0, 1); }
        else if (!strcmp(f, "var")) t->var = clampi(v, 0, 7);
        else if (!strcmp(f, "sound")) { t->sound = clampi(v, 0, C.npre - 1); t->ea = t->ed = t->es = t->er = t->eg = t->cut = t->res = t->sub = -1; sound_load(); }

        else if (!strcmp(f, "gate")) { t->gate = clampi(v, 10, 100); changed = 2; }
        else if (!strcmp(f, "swing")) { t->swing = clampi(v, 0, 100); changed = 2; }
        else if (!strcmp(f, "grnd")) { t->grnd = clampi(v, 0, 100); changed = 2; }
        else if (!strcmp(f, "ea") || !strcmp(f, "ed") || !strcmp(f, "es") || !strcmp(f, "er") || !strcmp(f, "eg") || !strcmp(f, "cut") || !strcmp(f, "res") || !strcmp(f, "sub")) {
            int k = !strcmp(f, "cut") ? 5 : !strcmp(f, "res") ? 6 : !strcmp(f, "sub") ? 7 : f[1] == 'a' ? 0 : f[1] == 'd' ? 1 : f[1] == 's' ? 2 : f[1] == 'r' ? 3 : 4;
            *env_field(k) = clampi(v, -1, 127); env_apply(); changed = 2;
        }
        else if (!strcmp(f, "eql")) { t->eql = clampi(v, 0, 127); changed = 2; }
        else if (!strcmp(f, "eqm")) { t->eqm = clampi(v, 0, 127); changed = 2; }
        else if (!strcmp(f, "eqh")) { t->eqh = clampi(v, 0, 127); changed = 2; }
        else if (!strcmp(f, "pan")) { t->pan = clampi(v, 0, 127); changed = 2; }
        else if (!strcmp(f, "engine")) { all_off(0); C.E->reset(C.eng); C.engine = clampi(v, 0, 1); C.nev = 0; }
        else if (!strcmp(f, "dly")) { t->dly = clampi(v, 0, 127); changed = 2; }
        else if (!strcmp(f, "rev")) { t->rev = clampi(v, 0, 127); changed = 2; }
        else if (!strcmp(f, "vol")) { t->vol = clampi(v, 0, 127); changed = 2; }
        else if (!strcmp(f, "center")) t->center = clampi(v, 55, 76);
        else if (!strcmp(f, "bpm10")) { t->bpm10 = clampi(v, 400, 3000); cm_set_bpm(t->bpm10 / 10.0); changed = 2; }
        else if (!strcmp(f, "last")) { t->last = clampi(v, 1, NSTEP); if (C.sel >= t->last) C.sel = t->last - 1; }
        else if (!strcmp(f, "steplen")) t->steplen = clampi(v, 0, NSTEPLEN - 1);
        else if (!strcmp(f, "name")) { const char *s = js_strv(m, "s"); if (s) snprintf(t->name, sizeof t->name, "%s", s); }
        else if (!strcmp(f, "chordmode")) { C.chordmode = !!v; live_chord_from_keys(); }
        else if (!strcmp(f, "outch")) C.outch = clampi(v, 1, 16);
        else if (!strcmp(f, "inch")) C.inch = clampi(v, 0, 16);
        else if (!strcmp(f, "outport")) { all_off(0); const char *s = js_strv(m, "s"); snprintf(C.outport, sizeof C.outport, "%s", s ? s : ""); }
        else if (!strcmp(f, "local")) { all_off(0); C.local = !!v; }
        else if (!strcmp(f, "strum")) C.strum_ms = clampi(v, 0, 80);
        else if (!strcmp(f, "oct")) { all_off(0); C.oct = clampi(v, -2, 2); }
        else if (!strcmp(f, "pchange")) C.pchange = clampi(v, 0, 2);
        else if (!strcmp(f, "hold")) { C.hold = !!v; if (!C.hold && !C.sustain && !C.nheld) { live_chord_from_keys(); refresh(0, 0); } cm_dirty(); return 1; }
        else return 0;
        /* what the held keys play may have changed (or was cut by all_off): sound and show it again */
        static const char *const RE[] = {"set", "key", "center", "chordmode", "oct", "local", "outport", "style", "var", "sound", "engine", 0};
        for (int i = 0; RE[i]; i++) if (!strcmp(f, RE[i])) { live_chord_from_keys(); refresh(0, 1); break; }
        if (strcmp(f, "outport") && strcmp(f, "engine") && strcmp(f, "chordmode") && strcmp(f, "outch") && strcmp(f, "inch") && strcmp(f, "local") && strcmp(f, "strum") && strcmp(f, "oct") && strcmp(f, "pchange")) t->used = 1;
        (void)changed;
        cm_dirty();
        return 1;
    }
    if (!strcmp(k, "pattern")) {
        v = clampi(v, 0, NPAT - 1);
        if (CK.playing) { C.next = v; } else { all_off(0); pattern_switch(v, 0); }
        cm_dirty(); return 1;
    }
    if (!strcmp(k, "edit")) { C.edit = v; if (C.edit) C.rec = 0; cm_dirty(); return 1; }
    if (!strcmp(k, "rec")) { C.rec = v; if (C.rec) C.edit = 0; cm_dirty(); return 1; }
    if (!strcmp(k, "sel")) {
        C.sel = clampi(v, 0, NSTEP - 1);
        if (C.sel >= t->last) t->last = C.sel + 1;
        step_t *s = &t->st[C.sel];
        if (!CK.playing && s->n > 0 && js_num(m, "hear", 0)) {   /* (audition the step's chord) */
            int nn[4]; for (int i = 0; i < s->n; i++) nn[i] = s->note[i];
            all_off(0); C.cur_n = 0;
            chord_set(0, nn, s->n, s->label, 0, 100, 1);
            C.seq_n = 0;
        }
        cm_dirty(); return 1;
    }
    if (!strcmp(k, "unhear")) { if (!CK.playing && !C.live_n) { block_stop(0); chord_set(0, NULL, 0, "", 0, 0, 0); } return 1; }
    if (!strcmp(k, "tie")) { step_tie(C.sel); C.sel = (C.sel + 1) % t->last; cm_dirty(); return 1; }
    if (!strcmp(k, "rest")) { step_t *s = &t->st[C.sel]; memset(s, 0, sizeof *s); s->style = -1; C.sel = (C.sel + 1) % t->last; cm_dirty(); return 1; }
    if (!strcmp(k, "stepstyle")) {                       /* the step's own phrase (-1 = the pattern's) */
        step_t *s = &t->st[C.sel]; s->style = (signed char)clampi(v, -1, ph_nstyles() - 1); s->var = (signed char)clampi((int)js_num(m, "var", 0), 0, 7);
        cm_dirty(); return 1;
    }
    if (!strcmp(k, "clearpat")) { all_off(0); char nm[16]; snprintf(nm, sizeof nm, "%s", t->name); int sd = t->sound; pat_default(t, C.cur); t->sound = sd; snprintf(t->name, sizeof t->name, "%s", nm); cm_dirty(); return 1; }
    if (!strcmp(k, "copypat")) {
        int to = clampi(v, 0, NPAT - 1);
        if (to != C.cur) { C.pat[to] = *t; snprintf(C.pat[to].name, sizeof C.pat[to].name, "%.12s+", t->name); }
        cm_dirty(); return 1;
    }
    if (!strcmp(k, "random")) {
        if (!CK.playing) all_off(0);
        gen_random();
        if (C.sel >= P()->last) C.sel = 0;
        refresh(0, 1);                                     /* (a held chord goes on, in the new style) */
        cm_dirty(); return 1;
    }
    if (!strcmp(k, "gen")) { C.gen = !!v; cm_dirty(); return 1; }
    if (!strcmp(k, "panic")) { all_off(0); C.nheld = 0; C.live_n = 0; C.cur_n = 0; C.E->reset(C.eng); return 1; }
    return 0;
}

/* ------------------------------------------------------------------ save / load */
static void ch_save(part_t *p, jb_t *b)
{
    (void)p;
    jb_key(b, "cur"); jb_int(b, C.cur);
    jb_key(b, "chordmode"); jb_int(b, C.chordmode);
    jb_key(b, "outch"); jb_int(b, C.outch);
    jb_key(b, "inch"); jb_int(b, C.inch);
    jb_key(b, "outport"); jb_str_utf8(b, C.outport);
    jb_key(b, "local"); jb_int(b, C.local);
    jb_key(b, "engine"); jb_int(b, C.engine);
    jb_key(b, "strum"); jb_int(b, C.strum_ms);
    jb_key(b, "oct"); jb_int(b, C.oct);
    jb_key(b, "pchange"); jb_int(b, C.pchange);
    jb_key(b, "patterns"); jb_ch(b, '[');
    for (int i = 0; i < NPAT; i++) {
        pat_t *t = &C.pat[i];
        if (i) jb_ch(b, ',');
        jb_ch(b, '{');
        jb_key(b, "name"); jb_str_utf8(b, t->name);
        { int si = t->sound < C.npre ? t->sound : 0, e = C.snd_eng[si]; char nm[96]; snprintf(nm, sizeof nm, "%s/%s", ENG_TAG[e], ENG[e]->preset_name(C.inst[e], C.pre[si]));
          jb_key(b, "sound_name"); jb_str_utf8(b, nm); }
        jb_key(b, "set_name"); jb_str_utf8(b, cs_name(t->set));
#define F(k) jb_key(b, #k); jb_int(b, t->k);
        F(last) F(steplen) F(bpm10) F(key) F(style) F(var) F(gate) F(swing) F(dly) F(rev) F(vol) F(center) F(used) F(grnd) F(ea) F(ed) F(es) F(er) F(eg) F(eql) F(eqm) F(eqh) F(pan)
#undef F
        jb_key(b, "fc"); jb_int(b, t->cut); jb_key(b, "fr"); jb_int(b, t->res); jb_key(b, "fs"); jb_int(b, t->sub);   /* (new keys: the old "cut"/"res" were offsets) */
        jb_key(b, "steps"); jb_ch(b, '[');
        int lastused = -1; for (int s = 0; s < NSTEP; s++) if (t->st[s].n || t->st[s].tie || t->st[s].style >= 0) lastused = s;
        for (int s = 0; s <= lastused; s++) { if (s) jb_ch(b, ','); step_json(b, &t->st[s]); }
        jb_ch(b, ']');
        jb_ch(b, '}');
    }
    jb_ch(b, ']');
}
static void ch_load(part_t *p, const js_t *o)
{
    (void)p;
    if (!o) return;
    C.chordmode = (int)js_num(o, "chordmode", 1); C.outch = clampi((int)js_num(o, "outch", 1), 1, 16);
    C.inch = clampi((int)js_num(o, "inch", 0), 0, 16); C.local = (int)js_num(o, "local", 1);
    C.strum_ms = clampi((int)js_num(o, "strum", 18), 0, 80); C.oct = clampi((int)js_num(o, "oct", 0), -2, 2);
    C.pchange = clampi((int)js_num(o, "pchange", 0), 0, 2);
    C.engine = clampi((int)js_num(o, "engine", 0), 0, 1);
    const char *op = js_strv(o, "outport"); snprintf(C.outport, sizeof C.outport, "%s", op ? op : "");
    const js_t *a = js_get(o, "patterns");
    if (a && a->type == JS_ARR) {
        int i = 0;
        for (const js_t *q = a->kid; q && i < NPAT; q = q->next, i++) {
            static pat_t tmp; pat_t *t = &tmp;
            pat_default(t, i);
            const char *nm = js_strv(q, "name"); if (nm) snprintf(t->name, sizeof t->name, "%s", nm);
            const char *sn = js_strv(q, "sound_name"); if (sn) t->sound = find_sound(sn);
            const char *se = js_strv(q, "set_name"); if (se) t->set = find_set(se);
#define F(k, lo, hi) t->k = clampi((int)js_num(q, #k, t->k), lo, hi);
            F(last, 1, NSTEP) F(steplen, 0, NSTEPLEN - 1) F(bpm10, 400, 3000) F(key, -12, 12) F(style, 0, ph_nstyles() - 1)
            F(var, 0, 7) F(gate, 10, 100) F(swing, 0, 100) F(dly, 0, 127) F(rev, 0, 127)
            F(vol, 0, 127) F(center, 55, 76) F(used, 0, 1) F(grnd, 0, 100) F(ea, -1, 127) F(ed, -1, 127) F(es, -1, 127) F(er, -1, 127) F(eg, -1, 127) F(eql, 0, 127) F(eqm, 0, 127) F(eqh, 0, 127) F(pan, 0, 127)
#undef F
            t->cut = clampi((int)js_num(q, "fc", -1), -1, 127); t->res = clampi((int)js_num(q, "fr", -1), -1, 127); t->sub = clampi((int)js_num(q, "fs", -1), -1, 127);
            const js_t *st = js_get(q, "steps");
            int s = 0;
            if (st && st->type == JS_ARR) for (const js_t *e = st->kid; e && s < NSTEP; e = e->next, s++) {
                step_t *x = &t->st[s];
                int n = (int)js_at(e, 0)->num;
                if (n < 0) { x->tie = 1; continue; }
                x->n = (signed char)clampi(n, 0, 4);
                const js_t *lb = js_at(e, 1); if (lb && lb->type == JS_STR) snprintf(x->label, sizeof x->label, "%s", lb->str);
                const js_t *nt = js_at(e, 2);
                for (int k = 0; k < x->n; k++) { const js_t *z = nt ? js_at(nt, k) : NULL; x->note[k] = (unsigned char)clampi(z ? (int)z->num : 60, 0, 127); }
                const js_t *sy = js_at(e, 3), *va = js_at(e, 4);
                x->style = (signed char)(sy ? clampi((int)sy->num, -1, ph_nstyles() - 1) : -1);
                x->var = (signed char)(va ? clampi((int)va->num, 0, 7) : 0);
            }
            if (t->used) C.pat[i] = *t;                    /* (an unused slot keeps its factory pattern) */
        }
    }
    C.cur = clampi((int)js_num(o, "cur", 0), 0, NPAT - 1);
    sound_load();
    cm_set_bpm(P()->bpm10 / 10.0);
}

static void ch_live(part_t *p, jb_t *b)
{
    (void)p;
    jb_key(b, "step"); jb_int(b, C.step);
    jb_key(b, "chord"); jb_str_utf8(b, C.cur_label);
    jb_key(b, "cn"); jb_ch(b, '[');
    for (int i = 0; i < C.cur_n; i++) { if (i) jb_ch(b, ','); jb_int(b, C.cur_note[i] + 12 * C.oct); }
    jb_ch(b, ']');
    jb_key(b, "snd"); jb_ch(b, '[');
    int f = 1; for (int n = 0; n < 128; n++) if (C.cnt[n] > 0) { if (!f) jb_ch(b, ','); f = 0; jb_int(b, n); }
    jb_ch(b, ']');
    jb_key(b, "held"); jb_ch(b, '[');
    for (int i = 0; i < C.nheld; i++) { if (i) jb_ch(b, ','); jb_int(b, C.held[i]); }
    jb_ch(b, ']');
    jb_key(b, "next"); jb_int(b, C.next);
    jb_key(b, "sel"); jb_int(b, C.sel);
    jb_key(b, "hold"); jb_int(b, C.hold);
}

const part_def PART_CHORD = {
    .kind = "chord", .name = "CHORD", .init = ch_init, .key = ch_key, .cc = ch_cc, .tick = ch_tick, .transport = ch_transport,
    .event = ch_event, .render = ch_render, .state = ch_state, .set = ch_set, .save = ch_save, .load = ch_load, .live = ch_live,
};
/* (for the core: the chord part's input channel, 0 = all) */
int chord_inch(void) { return C.inch; }
