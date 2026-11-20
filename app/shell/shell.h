// SPDX-License-Identifier: GPL-3.0-only
// Chordially: the C side of the Mac app (audio out, CoreMIDI, USB audio in, the data file, the log).
// main.swift calls these; the core is in ../core (cm.h).
#ifndef CM_SHELL_H
#define CM_SHELL_H
int sh_init(const char *log_path, const char *data_dir);
int sh_audio_start(void);
void sh_audio_stop(void);
int sh_midi_start(void);
void sh_msg(const char *json);          /* a message from the page */
char *sh_drain(void);                   /* the page's messages as one JSON array text, NULL when none; free() */
void sh_tick(void);                     /* about once a second, main thread: devices, autosave, log */
void sh_quit(void);
void sh_log(const char *fmt, ...);
void sh_note(const char *text);         /* (for Swift: one line into the log) */
void sh_page_log(const char *text);
void sh_post(const char *json);         /* a message for the page (any thread) */
void sh_set_insert(int (*fn)(float *L, float *R, int n));   /* insert.swift: the AU insert on the chord sound */
void sh_set_instr(int (*fn)(const int *ev, int nev, float *L, float *R, int n));   /* insert.swift: the AU instrument */
void sh_transport(double *bpm, int *playing, double *beat);  /* (audio thread: the plugins' host tempo / position) */
#endif
