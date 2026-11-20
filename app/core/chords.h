// SPDX-License-Identifier: GPL-3.0-only
#ifndef CM_CHORDS_H
#define CM_CHORDS_H
int cs_count(void);
const char *cs_name(int set);
const char *cs_symbol(int set, int key);
const char *cs_note_name(int pc);
int cs_voice(int set, int key, int tr, int center, int *out, char *label, int lmax);   /* 4 notes, bass first */
void cs_name_notes(const int *n, int cnt, char *out, int max);
#endif
