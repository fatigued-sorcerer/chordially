// SPDX-License-Identifier: GPL-3.0-only
// Chordially: what the core's files share. The app is a set of PARTS on one clock:
//   part 0  CHORD   chord sets, phrases, the 64-pattern chord sequencer, the AURA synth, MIDI out (part_chord.c)
//   part 1  S-1     an external box: its USB audio in, MIDI clock / transport out, optional note thru (part_ext.c)
//   part 2  T-8     the same for the T-8
// A new instrument (an internal S-1-style synth, a drum part ...) is one more part_def: it gets the same clock ticks
// (96 per quarter, free-running), transport, keys routed to it, scheduled events, and renders into the mix and the
// delay / reverb sends. Nothing else has to change: add it to PARTS[] in cm.c.
#ifndef CM_INT_H
#define CM_INT_H
#include <stdint.h>
#include "json.h"

#define SR 48000.0
#define PPQN 96
#define MAXF 1024

typedef struct part part_t;
typedef struct {
    const char *kind, *name;
    void (*init)(part_t *p);
    void (*key)(part_t *p, int frame, int note, int vel);        /* a key routed to this part (vel 0 = off) */
    void (*cc)(part_t *p, int frame, int cc, int val);           /* a controller / bend (cc -1 = pitch bend) */
    void (*tick)(part_t *p, int frame, long tick);               /* every clock tick, also while stopped */
    void (*transport)(part_t *p, int frame, int playing);
    void (*event)(part_t *p, int frame, int a, int b, int c);    /* a scheduled event came due */
    void (*render)(part_t *p, float *L, float *R, float *sd, float *sr, int n);   /* adds (sd, sr: mono sends) */
    void (*state)(part_t *p, jb_t *b);                           /* settings for the page: members of an object */
    int (*set)(part_t *p, const char *k, const js_t *m);         /* a message for this part; 1 = handled */
    void (*save)(part_t *p, jb_t *b);                            /* members of an object */
    void (*load)(part_t *p, const js_t *o);
    void (*live)(part_t *p, jb_t *b);                            /* 60 times a second: members of an object */
    void (*midi_in)(part_t *p, int port, int st, int d1, int d2);   /* (MIDI thread, under the lock) raw input, optional */
} part_def;
struct part { const part_def *d; int idx; void *s; float peak; };

typedef struct {
    double bpm;
    int playing;
    long tick;                 /* free-running tick count */
    long ptick;                /* ticks since PLAY (valid while playing) */
    double spt;                /* samples per tick */
    long long now;             /* absolute sample of the current block's first frame */
    int ext;                   /* 1: following an external clock right now */
} cm_clock_t;
extern cm_clock_t CK;
extern part_t PART[];
extern int NPART;

void cm_sched(long long at, part_t *p, int a, int b, int c);   /* an event for p at an absolute sample */
void cm_unsched(part_t *p);                                    /* drop p's pending events */
void cm_midi_port(const char *port, int frame, int st, int d1, int d2);   /* frame < 0: now (another thread) */
void cm_dirty(void);                                           /* the page needs the state again */
void cm_set_bpm(double bpm);
int cm_port_exists(const char *name, int out);
void cm_ports_json(jb_t *b);
extern int (*cm_ain)(int slot, int n, float *L, float *R);     /* the shell's audio-in reader (may be NULL) */
extern int (*cm_insert)(float *L, float *R, int n);
extern int (*cm_instr)(const int *ev, int nev, float *L, float *R, int n);   /* the shell's AU instrument (may be NULL) */            /* the shell's AU insert on the chord part (may be NULL) */
#endif
