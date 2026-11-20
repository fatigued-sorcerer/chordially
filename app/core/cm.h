// SPDX-License-Identifier: GPL-3.0-only
// Chordially core: what the shell (shell.c, main.swift) and the tests call.
#ifndef CM_H
#define CM_H
typedef void (*cm_midi_fn)(int port_uid, int frame, int st, int d1, int d2);   /* frame < 0: send now */
void cm_init(void);
void cm_set_midi_out(cm_midi_fn f);
void cm_set_ain(int (*f)(int slot, int n, float *L, float *R));
void cm_set_insert(int (*f)(float *L, float *R, int n));
void cm_set_instr(int (*f)(const int *ev, int nev, float *L, float *R, int n));   /* the AU instrument: renders the chord part when engine = 1 */   /* the AU insert on the chord sound (audio thread, in place) */
void cm_transport(double *bpm, int *playing, double *beat);   /* (audio thread, for plugins' host context) */
void cm_render(float *L, float *R, int n);          /* audio thread, 48 kHz; any n */
void cm_msg(const char *json);                      /* a message from the page */
void cm_midi_in(int port_uid, int st, int d1, int d2);   /* MIDI thread */
void cm_set_ports(const char *json);                /* {"ins":[{id,name}],"outs":[{id,name}]} */
char *cm_state(int force);                          /* the full state when it changed (or force), else NULL; free() */
char *cm_live(void);                                /* the live view (60 / s); free() */
char *cm_save(void);                                /* everything to keep; free() */
int cm_load(const char *json);
int cm_ext_count(void);
const char *cm_ext_dev(int k);                      /* the device name an EXT part looks for ("S-1") */
void cm_ext_present(int k, int on);
int cm_ext_audio(int k);                            /* AUDIO on: route its USB audio */
const char *cm_ext_adev(int k);                     /* the audio device chosen in the strip ("" = AUTO) */
int cm_ext_ach(int k);                              /* and its first input channel (0-based) */
void cm_set_adevs(const char *json);                /* [[name, inputs, aggregate], ...] for the strips' menus */
long long cm_samples(void);
long long cm_block_now(void);                        /* (inside the MIDI-out callback: the block's first sample) */
#endif
