// SPDX-License-Identifier: GPL-3.0-only
// Jammer: the instrument interface. Every internal instrument (a synth engine or a drum machine) is one jeng_def_t;
// each track that uses it owns one instance. Instances are independent (no globals): two tracks with the same engine
// are two instances. Everything runs on the audio thread except where noted; no allocation in render / midi.
//
// Parameters are a flat list (index 0..nparams-1), grouped in pages for the SOUND page; every parameter is an integer
// in [min, max] with a display text the engine writes ("1.2 kHz", "SAW", "40 %"). kind tells the UI how to draw it:
// JP_KNOB a range, JP_SWITCH a few named choices (min..max, each value has a text), JP_TOGGLE on/off.
// Macros: 8 per engine (MACRO 1..8 names + what they move), set relative to the preset (the core keeps the base).
// Presets are data inside the engine: preset_load(i) sets every parameter. Categories group them in the browser.
// Drums: nlanes() > 0. Lane l sounds when note lane_note(l) arrives (General MIDI-ish: 36 kick, 38 snare ...).
#ifndef JAMMER_JENG_H
#define JAMMER_JENG_H
#include <stdint.h>

enum { JP_KNOB, JP_SWITCH, JP_TOGGLE };
enum { JE_SYNTH, JE_DRUMS };

struct jviz;  /* (declared before use: the viz hook names it) */
typedef struct jeng_def {
    const char *id;            /* "grit", "noise", "boom", ... (saved in projects: never change) */
    const char *name;          /* "GRIT" */
    const char *family;        /* browser group: "Analog", "FM", "Physical", "Drums" ... */
    int kind;                  /* JE_SYNTH / JE_DRUMS */
    int poly;                  /* voices (1 = mono engine) */

    void *(*create)(double sample_rate);              /* main thread; NULL on failure */
    void (*destroy)(void *e);                         /* main thread */
    void (*reset)(void *e);                           /* all notes off, voices silent (no parameter change) */

    /* MIDI in: status bytes 0x80 note off, 0x90 note on (vel 0 = off), 0xB0 CC (1 mod, 64 sustain, 74 cutoff, 123 all
       notes off), 0xE0 pitch bend. Channel is ignored (one instance = one part). frame: offset in the next render block
       (0..n-1) where it takes effect (sample accurate). */
    void (*midi)(void *e, int frame, int st, int d1, int d2);
    /* n frames (<= 1024), written (not added) into L, R; silence when nothing sounds */
    void (*render)(void *e, float *L, float *R, int n);
    void (*tempo)(void *e, double bpm, int playing, double beat);   /* once a block, before render; may be NULL */

    int (*npages)(void *e);
    const char *(*page_name)(void *e, int p);         /* "OSC", "FILTER", "AMP ENV", "LFO", "FX" ... */
    int (*nparams)(void *e);
    int (*param_page)(void *e, int i);
    const char *(*param_name)(void *e, int i);
    int (*param_kind)(void *e, int i);                /* JP_KNOB / JP_SWITCH / JP_TOGGLE */
    int (*param_min)(void *e, int i);
    int (*param_max)(void *e, int i);
    int (*param_get)(void *e, int i);
    void (*param_set)(void *e, int i, int v);         /* audio-safe (smoothed inside where it would click) */
    const char *(*param_text)(void *e, int i, int v); /* the text of value v (not only the current one) */
    int (*param_basic)(void *e, int i);               /* 1: shown in the Basic view */

    int (*nmacros)(void *e);                          /* 8 */
    const char *(*macro_name)(void *e, int m);
    void (*macro_set)(void *e, int m, int v);         /* -64..+63, relative to the loaded preset */

    int (*npresets)(void *e);
    const char *(*preset_name)(void *e, int i);
    const char *(*preset_cat)(void *e, int i);        /* "Bass", "Pad", "Lead", "Pluck", "Keys", "Drone", "FX", "Arp", "Kit" */
    int (*preset_flags)(void *e, int i);              /* JF_HOLD (a true drone), JF_ARP (+ arp settings via preset_arp) */
    void (*preset_load)(void *e, int i);
    const char *(*preset_arp)(void *e, int i);        /* JSON of arp settings for JF_ARP presets ({"mode":0,"rate":5,...}), else NULL */

    int (*nlanes)(void *e);                           /* drums: 8..16; synths 0 */
    const char *(*lane_name)(void *e, int l);         /* "KICK", "SNARE", "CL HAT" ... */
    int (*lane_note)(void *e, int l);

    double (*peak)(void *e);                          /* last block's output peak (meters); may be NULL */

    /* Live state for the Sound page: the REAL envelope / LFO state of the newest sounding voice (or the one still
       releasing; the last one when all are idle). Audio thread, called by the core after render; v arrives zeroed.
       May be NULL (then the page shows no dot). See jviz_t below and briefs/viz.md. */
    void (*viz)(void *e, struct jviz *v);

    /* ---- J2-4 (all optional, may be NULL; briefs/engines-j24.md) ----
       Sample engines (KIT, CHOP): engine-specific commands, a JSON picture for the page, and state that presets and params
       cannot hold (the samples chosen, the slices), saved in jams. cmd / state_set run on the AUDIO thread: never block,
       hand file work to the engine's loader thread. info / state_get may be called on the audio thread too: build the text
       into an engine-owned buffer (valid until the next call), no allocation in the hot path is required for these. */
    void (*cmd)(void *e, const char *json);           /* {"pad":3,"file":"u:Kicks/k1.wav"}, {"chop":"grid","n":16}, ... */
    const char *(*info)(void *e);                     /* JSON for the page: sample lists, kit, waveform peaks, slices ... */
    const char *(*state_get)(void *e);                /* JSON object (or NULL): saved in the jam with the track */
    void (*state_set)(void *e, const char *json);     /* given back when the jam loads (after the preset and params) */
    /* Drums: per-lane sends. Replaces render when present: writes the dry mix into L, R and ADDS each lane's signal times
       its own send amounts into aux[0..5] = reverb L, R (NEBULA), delay L, R (DELAY), drive L, R (DRIVE). */
    void (*render_aux)(void *e, float *L, float *R, float *const *aux, int n);
} jeng_def_t;

/* ---- live envelope / LFO state (jeng_def_t.viz) ---- */
enum { JV_IDLE, JV_ATTACK, JV_DECAY, JV_SUSTAIN, JV_RELEASE };      /* (an AR or AD envelope skips the stages it has not) */
enum { JV_LIN, JV_EXP };                                             /* a segment's law: linear ramp, or RC (exponential approach) */
#define JV_MAX 4
typedef struct jviz {
    int nenv;
    struct { int page; int stage; float level; float sus; unsigned char alaw, dlaw, rlaw; } env[JV_MAX];
        /* page: index of the envelope's page ("AMP ENV", "FILTER ENV", "MOD ENV", ...); level: the envelope's output now,
           0..1 (1 = its peak); sus: its sustain level now 0..1; alaw/dlaw/rlaw: the law of attack / decay / release */
    int nlfo;
    struct { int page; float value, phase, hz; } lfo[JV_MAX];
        /* page: the LFO's page ("LFO 1" ...); value: its output now -1..1 (as the engine applies it, incl. shape / morph);
           phase: 0..1 within the cycle (0 = cycle start, as the drawn wave starts); hz: its rate now (after sync / mod) */
    int nplay; float play[8];                         /* sample engines: where the voices read now, 0..1 of the sample (KIT: pad index + fraction) */
} jviz_t;

enum { JF_HOLD = 1, JF_ARP = 2 };

/* the registry (core/engines.c): every engine compiled in, NULL-terminated */
extern const jeng_def_t *const JENGINES[];
const jeng_def_t *jeng_find(const char *id);
#endif
