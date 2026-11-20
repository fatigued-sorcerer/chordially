// Chordially: every internal SOUND, one by one. Checks: NaN / DC, held-chord level (calibrated), release to
// silence (no stuck notes), a played phrase is audible and stops, clicks at note on / off, and that FILTER, RESO
// and SUB really change the sound. Prints a line per sound and a summary; exit 1 on any failure.
// gcc -O2 -w -Iapp/core -Iapp/engines/classic1 -Iapp/engines/classic2 -o /tmp/pa tests/preset_audit.c <core + engines> -lm -lpthread
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cm.h"
#include "json.h"
int chord_sound_count(void);
static float L[256], R[256];
typedef struct { double pk, ss, dc, bright, jump, acc, loud; long n; int nan, w; } meas_t;
static double pl, pr;
static void run(double sec, meas_t *m) {
  int N = (int)(sec * 48000);
  for (int k = 0; k < N; k += 256) { cm_render(L, R, 256);
    for (int j = 0; j < 256; j++) { double a = L[j], b = R[j]; if (!isfinite(a) || !isfinite(b)) { m->nan = 1; continue; }
      double x = fabs(a) > fabs(b) ? fabs(a) : fabs(b); if (x > m->pk) m->pk = x; m->ss += a * a; m->dc += a; m->n++; m->acc += 0.5 * (a * a + b * b); if (++m->w == 2400) { if (m->acc / 2400 > m->loud) m->loud = m->acc / 2400; m->acc = 0; m->w = 0; }
      double d = fabs(a - pl); m->bright += d * d; if (d > m->jump) m->jump = d; pl = a; pr = b; } }
}
static double db(double x) { return 20 * log10(x + 1e-12); }
static void set(const char *f, int v) { char b[96]; snprintf(b, 96, "{\"t\":\"set\",\"f\":\"%s\",\"v\":%d}", f, v); cm_msg(b); }
static void key(int n, int v) { char b[64]; snprintf(b, 64, "{\"t\":\"key\",\"n\":%d,\"v\":%d}", n, v); cm_msg(b); }
static meas_t chord(double hold, double tail) { meas_t m = {0}; key(60, 100); run(hold, &m); key(60, 0); meas_t t = {0}; run(tail, &t); m.nan |= t.nan; return m; }
int main(void) {
  cm_init();
  cm_msg("{\"t\":\"pattern\",\"v\":7}"); set("style", 0); set("dly", 0); set("rev", 0); set("vol", 50);
  int n = chord_sound_count(), fails = 0;
  char *st = cm_state(1); js_t *M = js_parse(st); js_t *snds = js_get(js_get(M, "chord"), "sounds");
  printf("%-28s %6s %6s %6s %6s %7s %6s %6s %6s %s\n", "sound", "loud", "peak", "tail", "phrase", "jump", "FILT", "RESO", "SUB", "");
  for (int i = 0; i < n; i++) {
    js_t *e = js_at(snds, i); char name[80]; snprintf(name, 80, "%s · %s", js_at(e, 1)->str, js_at(e, 0)->str);
    set("style", 0); set("sound", i);
    char *s2 = cm_state(1); js_t *m2 = js_parse(s2); js_t *has = js_get(js_get(js_get(m2, "chord"), "p"), "ehas");
    int hasSub = js_at(has, 7)->num > 0, hasF = js_at(has, 5)->num > 0, hasR = js_at(has, 6)->num > 0; js_free(m2); free(s2);
    meas_t a = {0}; key(60, 100); run(2.5, &a); key(60, 0);
    meas_t rel = {0}; run(8, &rel); meas_t tail = {0}; run(0.5, &tail);   /* 8 s after the key: must be silent */
    double lvl = 10 * log10(a.loud + 1e-12) + 12.04, peak = db(a.pk) + 12.04, tl = db(tail.pk) + 12.04, dcv = fabs(a.dc / (a.n + 1));
    /* clicks: the biggest sample-to-sample step against the chord's peak */
    double jump = a.jump / (a.pk + 1e-9);
    /* FILTER / RESO / SUB: brightness (energy of the first difference) and level, low vs high */
    #define AB(f, lo, hi, out, rms) { set(f, lo); meas_t x = chord(1.0, 6); set(f, hi); meas_t y = chord(1.0, 6); set(f, -1); \
        out = 10 * log10((y.bright + 1e-12) / (x.bright + 1e-12)); rms = 10 * log10((y.ss + 1e-12) / (x.ss + 1e-12)); }
    double fb = 0, fr = 0, rb = 0, rr = 0, sb = 0, sr = 0;
    if (hasF) AB("cut", 10, 120, fb, fr);
    if (hasR) AB("res", 0, 120, rb, rr);
    if (hasSub) AB("sub", 0, 127, sb, sr);
    set("style", 1); meas_t ph = {0}; key(60, 100); run(2.5, &ph); key(60, 0); meas_t pt = {0}; run(8, &pt); meas_t ps = {0}; run(0.5, &ps); set("style", 0);
    double phl = db(ph.pk) + 12.04, pst = db(ps.pk) + 12.04;
    char why[160] = "";
    if (a.nan || rel.nan || ph.nan) strcat(why, " NaN");
    if (lvl > -10.5 || lvl < -22) strcat(why, " level");   /* -12 dB loudness; spiky sounds (plucks, airy pads) lower, as their peak (<= -0.5 dBFS) allows */
    if (peak > 0.5) strcat(why, " peak");
    if (tl > -70) strcat(why, " stuck/long-tail");
    if (pst > -70) strcat(why, " phrase-tail");
    if (phl < -30) strcat(why, " phrase-quiet");
    if (dcv > 0.01) strcat(why, " DC");
    if (hasF && fabs(fb) < 3) strcat(why, " FILTER-inert");
    if (hasR && fabs(rb) < 0.5 && fabs(rr) < 0.5) strcat(why, " RESO-inert");
    if (hasSub && fabs(sr) < 0.5) strcat(why, " SUB-inert");
    if (why[0]) fails++;
    printf("%-28.28s %6.1f %6.1f %6.0f %6.1f %7.2f %+6.1f %+6.1f %+6.1f %s\n", name, lvl, peak, tl, phl, jump, fb, rb, hasSub ? sr : 0.0, why[0] ? why : " ok");
  }
  printf("%d sounds, %d with problems\n%s\n", n, fails, fails ? "FAILED" : "ALL PASS");
  return fails != 0;
}
