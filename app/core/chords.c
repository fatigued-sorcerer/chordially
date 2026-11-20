// SPDX-License-Identifier: GPL-3.0-only
// Chordially: the chord sets and the voicing rule.
// A chord set gives one chord to each of the 12 keys (C .. B). The key pressed is always the BASS note (so Am7 on the
// C key sounds as Am7/C), the chord symbol decides the three notes above it. All sets are written in C; KEY transposes.
// Voicing: the three upper notes are chosen by importance for the chord quality (3rd, 7th, extensions before root and
// 5th), then placed in the one register band (around F#4) as the closest-position inversion. Because every chord of
// every set lands in the same band, moving between any two keys moves each voice only a few semitones: real voice
// leading, not parallel root positions. The bass sits between F2 and E3.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chords.h"

typedef struct { const char *q; signed char iv[7]; } qual_t;   /* intervals in priority order (-1 ends) */
static const qual_t Q[] = {
    {"M9", {4, 11, 2, 7, -1}}, {"M7#11", {4, 11, 6, 7, -1}}, {"M7", {4, 11, 7, 0, -1}},
    {"mM7", {3, 11, 7, 0, -1}}, {"m11", {3, 10, 5, 2, 7, -1}}, {"m9", {3, 10, 2, 7, -1}}, {"m7b5", {3, 6, 10, 0, -1}},
    {"m69", {3, 9, 2, 7, -1}}, {"m6", {3, 9, 7, 0, -1}}, {"m7", {3, 10, 7, 0, -1}}, {"madd9", {3, 2, 7, 0, -1}},
    {"m", {3, 0, 7, -1}}, {"dim7", {3, 6, 9, 0, -1}}, {"dim", {3, 6, 0, -1}}, {"aug", {4, 8, 0, -1}},
    {"7#5", {4, 8, 10, 0, -1}}, {"7b9", {4, 10, 1, 7, -1}}, {"7#9", {4, 10, 3, 7, -1}}, {"7sus4", {5, 10, 0, 7, -1}},
    {"9sus4", {5, 10, 2, 7, -1}}, {"13", {4, 10, 9, 2, -1}}, {"11", {10, 5, 2, 7, -1}}, {"9", {4, 10, 2, 7, -1}},
    {"7", {4, 10, 0, 7, -1}}, {"69", {4, 9, 2, 7, -1}}, {"6", {4, 9, 7, 0, -1}}, {"add9", {4, 2, 7, 0, -1}},
    {"sus2", {2, 7, 0, -1}}, {"sus4", {5, 7, 0, -1}}, {"5", {7, 0, 12, -1}}, {"", {4, 0, 7, -1}},
};
#define NQ ((int)(sizeof Q / sizeof Q[0]))

/* the sets: genre, then 12 chords for the keys C C# D D# E F F# G G# A A# B (bass = the key) */
static const char *const SETS[][13] = {
    {"Pop Bright", "C", "A7", "Dm7", "Eb", "C", "Fadd9", "D7", "G", "Fm", "Am", "Bb", "G"},
    {"Pop Ballad", "Cadd9", "A7", "Dm9", "EbM7", "Cadd9", "FM7", "D9", "G7sus4", "Fm6", "Am7", "C7", "Gadd9"},
    {"Pop Uplift", "Csus2", "A", "Dm7", "EbM9", "Em7", "Fadd9", "D", "Gsus4", "Ab", "Am9", "Bbadd9", "G"},
    {"Pop Minor", "Cm9", "DbM7", "Bb", "Eb", "C7", "Fm9", "D7", "G7", "AbM7", "F", "Bb", "G7"},
    {"Pop Anthem", "C", "Db", "Dm", "Eb", "Em", "F", "D", "G", "Ab", "Am", "Bb", "G"},
    {"Pop Sad", "Am9", "A7", "Dm9", "Ebdim7", "Em7", "FM9", "D9", "Gsus4", "AbM7", "Am7", "Bbadd9", "Esus4"},
    {"Synth Pop", "Cadd9", "Db", "Bb", "Eb", "C", "Fsus2", "D", "Gsus4", "Ab", "Am", "Bbsus2", "G"},
    {"City Pop", "CM9", "A7", "Dm9", "Eb9", "Em7", "FM9", "D9", "G13", "Ab9", "Am9", "Bb13", "E7#9"},
    {"Jazz ii-V", "CM9", "C#dim7", "Dm9", "D#dim7", "Em7", "FM7", "F#m7b5", "G13", "G#dim7", "A7b9", "Bb7", "Bm7b5"},
    {"Jazz Minor", "Cm9", "Db9", "Dm7b5", "EbM7#11", "Edim7", "Fm9", "F#dim7", "G7b9", "AbM7", "Am7b5", "Bb13", "Bdim7"},
    {"Jazz Turnaround", "C6", "A7", "Dm7", "Ebdim7", "Em7", "FM9", "F#m7b5", "G9", "Ab9", "Am9", "Bb9", "G7"},
    {"Jazz Ballad", "CM9", "DbM7", "Dm11", "Eb9", "E7b9", "Fm6", "F#m7b5", "G7sus4", "AbM9", "Am11", "BbM7", "B7b9"},
    {"Jazz Modal", "Dm11", "C#m7", "Dm9", "Ebm9", "Em11", "FM7#11", "F#m7", "G13", "Ab13", "Am11", "BbM9", "Bm7"},
    {"Jazz Blues", "C13", "C#dim7", "Dm7", "Ebdim7", "C7", "F9", "F#dim7", "G7#9", "Ab13", "A7b9", "Bb9", "B7#9"},
    {"Bossa", "C69", "C#dim7", "Dm9", "Eb9", "E7b9", "FM9", "F#m7b5", "G13", "Ab9", "Am7", "Bb7", "Bm7b5"},
    {"Bossa Minor", "Cm6", "Db7", "Dm7b5", "EbM7", "Edim7", "Fm7", "F#dim7", "G7b9", "AbM9", "Am7b5", "Bb13", "G7b9"},
    {"Neo Soul 1", "CM9", "C#m7", "Dm11", "EbM9", "Em9", "FM7", "F#m7b5", "G9sus4", "AbM7", "Am9", "BbM9", "Bm7b5"},
    {"Neo Soul 2", "Cm11", "DbM9", "Dm7b5", "EbM9", "E7#9", "Fm9", "F#dim7", "G7#9", "AbM9", "Am7b5", "Bb13", "Bdim7"},
    {"Neo Soul 3", "Em7", "A7", "Dm9", "Ebm9", "Em9", "FM9", "D9", "G9sus4", "Fm9", "Am11", "C9", "G13"},
    {"Neo Soul 4", "Am11", "DbM7", "Dm11", "EbM7#11", "Em11", "Fm9", "D13", "Em7", "AbM9", "Am9", "BbM7#11", "E7#9"},
    {"Gospel", "C", "A7", "Dm7", "Ebdim7", "C", "F", "F#dim7", "G7sus4", "Abdim7", "Am7", "C7", "G"},
    {"Gospel R&B", "CM9", "A7b9", "Dm9", "Ebdim7", "Em7", "FM9", "D9", "G13", "Ab13", "Am9", "Bb13", "E7#9"},
    {"Lofi", "FM9", "C#m7", "Dm9", "EbM9", "Em7", "FM7", "D9", "G9sus4", "AbM7", "Am11", "BbM9", "E7#9"},
    {"Lofi Minor", "Cm9", "DbM9", "Dm7b5", "EbM7", "Edim7", "Fm9", "Ab", "G7#9", "AbM7", "F9", "Bbsus2", "G7b9"},
    {"Funk", "C9", "C#9", "Dm7", "Eb9", "E7#9", "F9", "F#dim7", "G9", "Ab9", "Am7", "Bb9", "B7#9"},
    {"Funk Minor", "Cm7", "Db9", "Dm7b5", "Eb9", "Em7", "F9", "F#dim7", "G7#9", "Ab7", "Am7b5", "Bb9", "G7b9"},
    {"Blues", "C7", "C#dim7", "D7", "Ebdim7", "C7", "F7", "F#dim7", "G7", "Ab7", "A7", "Bb7", "G7"},
    {"Soul", "C", "A7", "Dm7", "Eb", "C", "F", "D7", "G7", "Fm6", "Am7", "Bb", "G7"},
    {"House", "Cm7", "Db", "Dm7", "Eb", "Em7", "Fm7", "D7", "Gm7", "Ab", "Am7", "Bb", "G"},
    {"Deep House", "Cm9", "DbM9", "Dm9", "EbM9", "Em9", "Fm9", "D9", "Gm9", "AbM9", "Am9", "BbM9", "G13"},
    {"Jazz House", "CM9", "C#m11", "Dm9", "Ebm9", "Em11", "FM9", "F#m11", "G9sus4", "AbM9", "Am11", "BbM9", "Bm11"},
    {"Organ House", "Cm7", "Dbsus2", "Dm7", "Eb6", "Em7", "Fm7", "D7", "Gm7", "Ab6", "Am7", "Bb6", "G7"},
    {"Techno", "Cm", "Db", "Dm", "Eb", "Em", "Fm", "D", "Gm", "Ab", "Am", "Bb", "G"},
    {"Dub Techno", "Cm9", "Dbsus2", "Dm9", "Eb6", "Em9", "Fm9", "D9", "Gm9", "AbM9", "Am9", "Bbsus2", "G"},
    {"Trance", "Cm", "Db", "Bb", "Eb", "C", "Fm", "D", "Gm", "Ab", "F", "Bb", "G"},
    {"Trance Uplift", "Cadd9", "Db", "Dm", "Ebsus2", "Em", "Fsus2", "D", "Gsus4", "Absus2", "Am", "Bbsus2", "G"},
    {"Psy Minor", "Am", "A", "Dm", "Bb", "Am", "F", "D", "G", "E", "Am", "Bb", "E7"},
    {"Psy Phrygian", "Cm", "Db", "Bb", "Eb", "C", "Fm", "Db", "Gm", "Ab", "F", "Bbm", "G"},
    {"Goa", "Em", "A", "Dm", "Eb", "Em", "F", "D", "G", "E", "Am", "Bb", "Em"},
    {"EDM", "C", "Db", "Dm", "Eb", "Em", "F", "D", "G", "Ab", "Am", "Bb", "G"},
    {"EDM Minor", "Cm", "Db", "Bb", "Eb", "C", "Fm", "D", "G", "Ab", "F", "Bb", "G"},
    {"Future Bass", "CM9", "Db", "Dm9", "EbM9", "Em7", "FM7", "D9", "G9sus4", "AbM7", "Am9", "BbM7", "G"},
    {"Synthwave", "Am", "A", "Dm", "Eb", "Em", "F", "D", "G", "Ab", "Am", "Bb", "Esus4"},
    {"Synthwave 2", "Cm9", "Db", "Bb", "Eb", "C7", "Fm9", "D", "Gsus4", "AbM7", "F9", "Bbsus2", "G"},
    {"Chillout", "CM7", "C#m7", "Dm9", "EbM7", "Em9", "FM7", "F#m7b5", "G9sus4", "AbM7", "Am9", "BbM7", "Bm7"},
    {"Downtempo", "Cm9", "DbM7", "Dm7b5", "EbM9", "Edim7", "Fm11", "F#dim7", "Gm9", "AbM7#11", "F9", "Bb13", "G7b9"},
    {"Ambient", "Csus2", "C#m7", "Dsus2", "Ebsus2", "Esus4", "Fsus2", "F#m7", "Gsus2", "Absus2", "Asus4", "Bbsus2", "Bsus4"},
    {"Ambient Lydian", "CM7#11", "DbM7", "D9", "EbM7#11", "Em9", "FM7#11", "D9", "GM9", "AbM7#11", "Am9", "BbM7#11", "Bm7"},
    {"Ambient Minor", "Cmadd9", "DbM7", "Dm11", "EbM9", "Em11", "Fmadd9", "D9", "Gm11", "AbM9", "Am11", "BbM9", "Gsus2"},
    {"Healing", "Cadd9", "Dbsus2", "Dsus2", "EbM9", "Em11", "FM7#11", "Dadd9", "Gsus2", "AbM9", "Am11", "BbM9", "Esus4"},
    {"Cinematic", "Cm", "Db", "Bb", "Eb", "C", "Fm", "D", "G", "Ab", "F", "Bb", "G"},
    {"Cinematic Epic", "Cm", "DbM7", "Dm7b5", "Eb", "Em", "Fm", "D", "Gsus4", "Ab", "Am", "Bb", "G"},
    {"Cinematic Horror", "Cm", "Db", "Ddim", "Ebaug", "Em", "Fm", "F#dim7", "Gaug", "Ab", "Am", "Bbm", "Bdim"},
    {"Cinematic Hope", "Csus2", "A", "Dm", "EbM7", "Em", "Fadd9", "D", "G", "Ab", "Am", "Bbadd9", "G"},
    {"Classical", "C", "A7", "Dm", "Ebdim7", "C", "F", "D7", "G7", "Fm", "Am", "Gm", "G"},
    {"Baroque", "C", "A7", "Dm", "Ebdim7", "Em", "F", "D7", "G", "Abdim7", "Am", "C7", "G"},
    {"Classical Minor", "Cm", "Db", "Dm7b5", "Eb", "C7", "Fm", "D7", "G7", "Ab", "F", "Bb", "G7"},
    {"Romantic", "C", "A7b9", "Dm", "Ebdim7", "Em", "F", "F#dim7", "G7", "Ab", "Am", "C7", "G7"},
    {"Reggae", "C", "A7", "Dm", "Eb", "Em", "F", "D7", "G", "Ab", "Am", "Bb", "G"},
    {"Latin", "Cm", "Db7", "Dm7b5", "Eb", "C7", "Fm", "D7", "G7", "Ab", "F7", "Bb", "G7"},
    {"Rock", "C5", "Db5", "D5", "Eb5", "E5", "F5", "F#5", "G5", "Ab5", "A5", "Bb5", "B5"},
    {"Rock Chords", "C", "A", "Dm", "Eb", "Em", "F", "D", "G", "Ab", "Am", "Bb", "G"},
    {"Dorian", "Dm9", "C#m7", "Dm7", "Ebm7", "Em7", "FM7", "D7", "G7", "Ab", "Am7", "BbM7", "G"},
    {"Mixolydian", "C", "Db", "Dm7", "Eb", "Em7b5", "F", "D", "Gm7", "Ab", "Am7", "Bb", "G"},
};
#define NSETS ((int)(sizeof SETS / sizeof SETS[0]))

static const char *NOTE[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static int root_pc(const char *s, int *len)
{
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
    if (s[0] < 'A' || s[0] > 'G') { *len = 0; return 0; }
    int pc = base[s[0] - 'A'], n = 1;
    if (s[1] == '#') { pc++; n++; } else if (s[1] == 'b') { pc--; n++; }
    *len = n;
    return (pc + 12) % 12;
}

int cs_count(void) { return NSETS; }
const char *cs_name(int set) { return set >= 0 && set < NSETS ? SETS[set][0] : ""; }
const char *cs_symbol(int set, int key) { return set >= 0 && set < NSETS && key >= 0 && key < 12 ? SETS[set][key + 1] : ""; }
const char *cs_note_name(int pc) { return NOTE[((pc % 12) + 12) % 12]; }

/* the chord on key (0..11) of set, transposed by tr semitones: notes ascending (bass first), returns the count */
int cs_voice(int set, int key, int tr, int center, int *out, char *label, int lmax)
{
    if (set < 0 || set >= NSETS || key < 0 || key > 11) return 0;
    const char *sym = SETS[set][key + 1];
    int rl, root = root_pc(sym, &rl);
    const char *qs = sym + rl;
    const qual_t *q = &Q[NQ - 1];
    for (int i = 0; i < NQ; i++) if (!strcmp(qs, Q[i].q)) { q = &Q[i]; break; }
    int bass_pc = key;
    /* the upper three: by priority, never the bass pitch class; then the root, then repeats */
    int up[3], nu = 0;
    for (int i = 0; q->iv[i] >= 0 && nu < 3; i++) {
        int pc = (root + q->iv[i]) % 12;
        if (pc == bass_pc) continue;
        int dup = 0; for (int k = 0; k < nu; k++) if (up[k] == pc) dup = 1;
        if (!dup) up[nu++] = pc;
    }
    /* (a triad: complete it with the tone the bass already has, then the root again) */
    for (int i = 0; q->iv[i] >= 0 && nu < 3; i++) {
        int pc = (root + q->iv[i]) % 12, dup = 0;
        for (int k = 0; k < nu; k++) if (up[k] == pc) dup = 1;
        if (!dup) up[nu++] = pc;
    }
    while (nu < 3) up[nu++] = root;
    for (int k = 0; k < 3; k++) up[k] = (up[k] + tr + 120) % 12;
    bass_pc = (bass_pc + tr + 120) % 12;
    /* place: every octave choice of the three in 52..84, closest position (span <= 12), nearest the centre */
    int best[3] = {60, 64, 67}; double bs = 1e9;
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int c = 0; c < 3; c++) {
        int n[3] = {up[0] + 48 + 12 * a, up[1] + 48 + 12 * b, up[2] + 48 + 12 * c};
        int lo = n[0], hi = n[0];
        for (int k = 1; k < 3; k++) { if (n[k] < lo) lo = n[k]; if (n[k] > hi) hi = n[k]; }
        if (hi - lo > 12 || lo < 52 || hi > 84) continue;
        if (n[0] == n[1] || n[1] == n[2] || n[0] == n[2]) continue;
        double mean = (n[0] + n[1] + n[2]) / 3.0;
        double s = (mean - center) * (mean - center) + 0.15 * (hi - lo);
        if (s < bs) { bs = s; memcpy(best, n, sizeof best); }
    }
    /* sort */
    for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) if (best[j] < best[i]) { int t = best[i]; best[i] = best[j]; best[j] = t; }
    int bass = 41 + ((bass_pc - 5 + 12) % 12);           /* F2 .. E3 */
    while (bass > best[0] - 3) bass -= 12;
    out[0] = bass; out[1] = best[0]; out[2] = best[1]; out[3] = best[2];
    if (label) {
        int rp = (root + tr + 120) % 12;
        if (rp == bass_pc) snprintf(label, (size_t)lmax, "%s%s", NOTE[rp], qs);
        else snprintf(label, (size_t)lmax, "%s%s/%s", NOTE[rp], qs, NOTE[bass_pc]);
    }
    return 4;
}

/* a name for any notes (for chords typed note by note): the best-matching symbol, else the note list */
void cs_name_notes(const int *n, int cnt, char *out, int max)
{
    out[0] = 0;
    if (cnt <= 0) return;
    if (cnt == 1) { snprintf(out, (size_t)max, "%s", NOTE[n[0] % 12]); return; }
    int lo = n[0]; for (int i = 1; i < cnt; i++) if (n[i] < lo) lo = n[i];
    int mask = 0; for (int i = 0; i < cnt; i++) mask |= 1 << (n[i] % 12);
    int bestq = -1, bestr = 0, bestmiss = 99;
    for (int r = 0; r < 12; r++) for (int qi = 0; qi < NQ; qi++) {
        int qm = 0; for (int k = 0; Q[qi].iv[k] >= 0; k++) qm |= 1 << ((r + Q[qi].iv[k]) % 12);
        if ((mask & ~qm) & 0xfff) continue;                     /* a note the chord has not */
        int miss = __builtin_popcount(qm & ~mask & 0xfff);
        if (!(mask & (1 << r))) miss += 1;                       /* (rootless: a little worse) */
        if (miss < bestmiss || (miss == bestmiss && r == lo % 12)) { bestmiss = miss; bestq = qi; bestr = r; }
    }
    if (bestq < 0 || bestmiss > 1) {
        int k = 0;
        for (int i = 0; i < cnt && k < max - 4; i++) k += snprintf(out + k, (size_t)(max - k), "%s%s", i ? " " : "", NOTE[n[i] % 12]);
        return;
    }
    if (bestr == lo % 12) snprintf(out, (size_t)max, "%s%s", NOTE[bestr], Q[bestq].q);
    else snprintf(out, (size_t)max, "%s%s/%s", NOTE[bestr], Q[bestq].q, NOTE[lo % 12]);
}
