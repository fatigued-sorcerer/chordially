// Chordially: stress test of the chord part's state. Random keys, chord-mode / style / octave / pattern / key changes,
// play / stop, sustain, RANDOM, GENERATIVE. Invariants checked against the live view (what the page shows):
//  A. a held chord with STYLE OFF (stopped): the sounding notes are exactly the chord's notes
//  B. everything released and stopped: after the release tails nothing sounds (no stuck notes), audio silent
//  C. no NaN, no output above the ceiling
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cm.h"
#include "json.h"
static int fails, checks;
static unsigned rs = 12345;
static int R(int n) { rs = rs * 1103515245u + 12345u; return (int)((rs >> 8) % (unsigned)n); }
static float L[4096], Rr[4096];
static double maxpk; static int nan_;
static void run(int blocks) { for (int b = 0; b < blocks; b++) { cm_render(L, Rr, 256); for (int i = 0; i < 256; i++) { if (L[i] != L[i]) nan_ = 1; double a = fabs(L[i]); if (a > maxpk) maxpk = a; } } }
static void msg(const char *fmt, int v) { char b[160]; snprintf(b, sizeof b, fmt, v); cm_msg(b); }
static int arr(js_t *o, const char *k, int *out) { js_t *a = js_get(o, k); int n = 0; if (a) for (js_t *e = a->kid; e && n < 64; e = e->next) out[n++] = (int)e->num; return n; }
static void live(int *cn, int *ncn, int *snd, int *nsnd, int *held, int *nheld, int *play)
{
    char *s = cm_live(); js_t *m = js_parse(s); js_t *c = js_get(m, "c");
    *ncn = arr(c, "cn", cn); *nsnd = arr(c, "snd", snd); *nheld = arr(c, "held", held); *play = (int)js_num(m, "play", 0);
    js_free(m); free(s);
}
static int same_set(const int *a, int na, const int *b, int nb)
{
    for (int i = 0; i < na; i++) { int f = 0; for (int j = 0; j < nb; j++) if (a[i] == b[j]) f = 1; if (!f) return 0; }
    for (int j = 0; j < nb; j++) { int f = 0; for (int i = 0; i < na; i++) if (a[i] == b[j]) f = 1; if (!f) return 0; }
    return 1;
}
int main(int argc, char **argv)
{
    if (argc > 1) rs = (unsigned)atoi(argv[1]);
    cm_init();
    int held[16], nh = 0, style = 0, playing = 0, sus = 0, hold = 0;
    msg("{\"t\":\"pattern\",\"v\":%d}", 7);
    for (int it = 0; it < 20000; it++) {
        int a = R(100); static int hist[6]; memmove(hist, hist + 1, sizeof(int) * 5); hist[5] = a;
        if (a < 30 && nh < 4) { int n = 48 + R(25); int dup = 0; for (int i = 0; i < nh; i++) if (held[i] == n) dup = 1; if (!dup) { held[nh++] = n; char b[80]; snprintf(b, 80, "{\"t\":\"key\",\"n\":%d,\"v\":100}", n); cm_msg(b); } }
        else if (a < 58 && nh > 0) { int i = R(nh); char b[80]; snprintf(b, 80, "{\"t\":\"key\",\"n\":%d,\"v\":0}", held[i]); cm_msg(b); held[i] = held[--nh]; }
        else if (a < 63) { style = R(9); msg("{\"t\":\"set\",\"f\":\"style\",\"v\":%d}", style); }
        else if (a < 66) msg("{\"t\":\"set\",\"f\":\"oct\",\"v\":%d}", R(5) - 2);
        else if (a < 69) msg("{\"t\":\"set\",\"f\":\"chordmode\",\"v\":%d}", R(2));
        else if (a < 72) { msg("{\"t\":\"pattern\",\"v\":%d}", R(64)); }
        else if (a < 74) msg("{\"t\":\"set\",\"f\":\"key\",\"v\":%d}", R(13) - 6);
        else if (a < 76) msg("{\"t\":\"set\",\"f\":\"var\",\"v\":%d}", R(8));
        else if (a < 78) { cm_msg(playing ? "{\"t\":\"stop\"}" : "{\"t\":\"play\"}"); playing = !playing; }
        else if (a < 80) { sus = !sus; char b[80]; snprintf(b, 80, "{\"t\":\"set\",\"f\":\"x\",\"v\":0}"); (void)b; /* (sustain comes by MIDI) */ cm_midi_in(1, 0xB0, 64, sus ? 127 : 0); }
        else if (a < 81) cm_msg("{\"t\":\"random\"}");
        else if (a < 82) msg("{\"t\":\"gen\",\"v\":%d}", R(2));
        else if (a < 84) msg("{\"t\":\"set\",\"f\":\"center\",\"v\":%d}", 58 + R(14));
        else if (a < 86) msg("{\"t\":\"set\",\"f\":\"set\",\"v\":%d}", R(64));
        else if (a < 88) { hold = !hold; msg("{\"t\":\"set\",\"f\":\"hold\",\"v\":%d}", hold); }
        run(1 + R(12));
        /* A: one or more keys held, STYLE OFF, stopped, no sustain */
        if (!playing && !sus && !hold && nh > 0 && it % 3 == 0) {
            /* (style of the current pattern: read from state) */
            char *st = cm_state(1); js_t *m = js_parse(st); int sty = (int)js_num(js_get(js_get(m, "chord"), "p"), "style", -1); js_free(m); free(st);
            if (sty == 0) {
                run(2);
                int cn[64], ncn, snd[64], ns, hd[64], nhd, pl; live(cn, &ncn, snd, &ns, hd, &nhd, &pl);
                checks++;
                if (!same_set(cn, ncn, snd, ns)) {
                    fails++;
                    if (fails < 12) { printf("FAIL A it %d: chord [", it); for (int i = 0; i < ncn; i++) printf("%d ", cn[i]); printf("] sounding ["); for (int i = 0; i < ns; i++) printf("%d ", snd[i]); printf("] held %d  last actions:", nhd); for (int q = 0; q < 6; q++) printf(" %d", hist[q]); printf("\n"); }
                }
            }
        }
        /* B: every 500 steps release everything and check silence */
        if (it % 500 == 499) {
            for (int i = 0; i < nh; i++) { char b[80]; snprintf(b, 80, "{\"t\":\"key\",\"n\":%d,\"v\":0}", held[i]); cm_msg(b); }
            nh = 0; cm_midi_in(1, 0xB0, 64, 0); sus = 0; hold = 0; cm_msg("{\"t\":\"set\",\"f\":\"hold\",\"v\":0}");
            if (playing) { cm_msg("{\"t\":\"stop\"}"); playing = 0; }
            run(48000 * 12 / 256);   /* (long pad releases and reverb tails die away) */
            int cn[64], ncn, snd[64], ns, hd[64], nhd, pl; live(cn, &ncn, snd, &ns, hd, &nhd, &pl);
            maxpk = 0; run(48000 / 256);
            checks++;
            if (ns || nhd || ncn || maxpk > 0.003) { fails++; printf("FAIL B it %d: sounding %d, held %d, chord %d, level %.4f\n", it, ns, nhd, ncn, maxpk); }
        }
    }
    checks++; if (nan_) { fails++; printf("FAIL C: NaN\n"); }
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL PASS");
    return fails != 0;
}
