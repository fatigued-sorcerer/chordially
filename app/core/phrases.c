// SPDX-License-Identifier: GPL-3.0-only
// Chordially: the phrase table (STYLE x VARIATION). A phrase is one text line on a sixteenth-note grid:
//   1..4  a chord note (1 = the bass, 2..4 = the upper notes), 5..8 the same an octave up
//   x X   the whole chord (X accented)          c C   the upper notes only (no bass)
//   d D   strum down (low to high)              u U   strum up (high to low)
//   r     a random upper note                   -     the previous event goes on       .  rest
// An event lasts until the next symbol that is not '-' (times the GATE knob). The phrase loops; STYLE 0 = OFF: the
// chord simply sounds while the key is held (or for the whole step).
#include "phrases.h"

typedef struct { const char *name; const char *var[8]; const char *vname[8]; } style_t;
static const style_t S[] = {
    {"OFF", {0}, {0}},
    {"ARP 8", {"2-3-4-6-", "2-3-4-6-7-6-4-3-", "6-4-3-2-", "2-3-4-6-7-8-", "1-2-3-4-", "2-4-3-6-", "4-3-2-1-", "r-r-r-r-"},
              {"UP", "UP DOWN", "DOWN", "UP 2 OCT", "UP + BASS", "LEAP", "DOWN + BASS", "RANDOM"}},
    {"ARP 16", {"2346", "23467643", "6432", "234678", "1234", "2324", "4323", "rrrr"},
               {"UP", "UP DOWN", "DOWN", "UP 2 OCT", "UP + BASS", "TRILL", "TURN", "RANDOM"}},
    {"PSY", {".234.234.234.234", "1.231.231.231.23", "2.3.4.3.6.4.3.2.", ".222.333.444.333", "1234323412343234",
             ".2.3.2.4.2.3.2.6", "2346432123464321", ".c.c.c.c.C.c.c.c"},
            {"ROLL", "GALLOP", "WAVE", "TRIPLE", "SPIRAL", "PEDAL", "RIPPLE", "OFFBEAT"}},
    {"PULSE", {"x---------------", "x-------x-------", "x---x---x---x---", "x-x-x-x-x-x-x-x-", "X-xxX-xxX-xxX-xx",
               "x--x--x--x--x-x-", "..x-..x-..x-..x-", "x--x--x-x--x--x-"},
              {"WHOLE", "HALF", "QUARTER", "EIGHTH", "GALLOP", "DOTTED", "OFFBEAT", "3-3-2"}},
    {"RHYTHM", {"1.C-1.C-1.C-1.C-", "..c...c...c...c.", "x.cx.c.cx.c.cxc.", "x--c------------", "x--x--x---c-c-..",
                "X.c.c.X.c.c.X.c.", "x-.cx-.c", "1-c-1-c-1-c-1-c-"},
               {"HOUSE", "SKANK", "FUNK", "CHARLESTON", "TRESILLO", "GATE", "STAB", "OOM-PAH"}},
    {"STRUM", {"d---------------", "d-------u-------", "d---d---d---d---", "d---u---d---u---", "d-u-d-u-d-u-d-u-",
               "D--d--d-D--d--d-", "u---------------", "d-------d---u---"},
              {"SLOW", "DOWN UP", "BEATS", "BEAT DOWN UP", "EIGHTHS", "DOTTED", "SLOW UP", "BALLAD"}},
    {"STRUM RHY", {"D-du-udu", "D.d.u.du.ud.u.du", "D--u--d-u-d-u-d-", "D-u-.-u-D-u-.-ud", "D.du.udu.udu.udu",
                   "D-.-u-.-D-.-u-du", "D-dud-udD-dud-ud", "d.u.D.u.d.u.D.ud"},
                  {"FOLK", "POP", "SYNC", "ROCK", "SIXTEENTHS", "REGGAE", "COUNTRY", "LATIN"}},
    {"BROKEN", {"1424", "1-4-3-4-2-4-3-4-", "1-c-c-c-", "1---c---1---c---", "1-3-4-6-4-3-", "1534253415342534",
                "1-5-2-3-4-3-2-5-", "15c-15c-15c-1c5c"},
               {"ALBERTI", "BALLAD", "BASS + CHORD", "WALTZ", "OPEN", "CASCADE", "NOCTURNE", "STRIDE"}},
};
#define NS ((int)(sizeof S / sizeof S[0]))

int ph_nstyles(void) { return NS; }
const char *ph_style(int s) { return s >= 0 && s < NS ? S[s].name : ""; }
const char *ph_var_name(int s, int v) { return s > 0 && s < NS && v >= 0 && v < 8 ? S[s].vname[v] : ""; }
const char *ph_text(int s, int v) { return s > 0 && s < NS && v >= 0 && v < 8 ? S[s].var[v] : 0; }
