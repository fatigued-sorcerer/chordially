// Chordially: measures every internal sound with a held C chord (STYLE OFF, no delay / reverb) and prints the
// level table for core/snd_levels.h (target: held C chord, loudest 50 ms RMS -12 dB at VOLUME 100).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cm.h"
#include "json.h"
int chord_sound_count(void);
#define TARGET -12.0   /* loudest 50 ms RMS of a held chord, dB at VOLUME 100 */
static float L[48000 * 3], R[48000 * 3];
/* loudness of a held C chord: the loudest 50 ms RMS window within 2.5 s (stable, unlike the sample peak, which
   jumps by several dB between strikes with the chorus / drift phases); dB at VOLUME 100 */
static double lastpk;
static double meas(int style) {
  (void)style; double pk = 0;
  cm_msg("{\"t\":\"key\",\"n\":60,\"v\":100}");
  int N = 48000 * 5 / 2, W = 2400, w = 0; double acc = 0, best = 0;
  for (int k = 0; k < N; k += 256) { cm_render(L, R, 256); for (int j = 0; j < 256; j++) { acc += 0.5 * (L[j] * L[j] + R[j] * R[j]); double a = fabs(L[j]) > fabs(R[j]) ? fabs(L[j]) : fabs(R[j]); if (a > pk) pk = a; if (++w == W) { if (acc / W > best) best = acc / W; acc = 0; w = 0; } } }
  cm_msg("{\"t\":\"key\",\"n\":60,\"v\":0}"); for (int k = 0; k < 48000 * 6; k += 256) cm_render(L, R, 256);
  lastpk = 20 * log10(pk + 1e-12) + 12.04;
  return 10 * log10(best + 1e-12) + 12.04;   /* (measured at VOLUME 50 = -12 dB, clear of the master limiter) */
}
int main(int argc, char **argv) {
  cm_init(); int report = argc > 1;
  cm_msg("{\"t\":\"pattern\",\"v\":7}"); cm_msg("{\"t\":\"set\",\"f\":\"style\",\"v\":0}"); cm_msg("{\"t\":\"set\",\"f\":\"dly\",\"v\":0}"); cm_msg("{\"t\":\"set\",\"f\":\"rev\",\"v\":0}"); cm_msg("{\"t\":\"set\",\"f\":\"vol\",\"v\":50}");
  int n = chord_sound_count();
  for (int i = 0; i < n; i++) {
    char b[80]; snprintf(b, 80, "{\"t\":\"set\",\"f\":\"sound\",\"v\":%d}", i); cm_msg(b);
    char *st = cm_state(1); js_t *m = js_parse(st); js_t *snds = js_get(js_get(m, "chord"), "sounds"); js_t *e = js_at(snds, i);
    char name[96]; const char *tag = js_at(e, 2)->str; snprintf(name, 96, "%s/%s", tag, js_at(e, 0)->str);
    double dc = 0, dp = -200; for (int r = 0; r < 3; r++) { dc += meas(0) / 3; if (lastpk > dp) dp = lastpk; }   /* loudness: mean of 3 strikes; peak: the highest */
    if (report) printf("%-34s %6.1f dB  peak %5.1f\n", name, dc, dp);
    else { double g = fmin(pow(10, (TARGET - dc) / 20), pow(10, (-0.5 - dp) / 20));   /* and the peak stays under -0.5 dBFS (generate from an empty table) */ printf("    {\"%s\", %.3ff},\n", name, g); }
    js_free(m); free(st);
  }
}
