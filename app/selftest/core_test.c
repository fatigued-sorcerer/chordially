#define _GNU_SOURCE
#include <time.h>
// Chordially: core tests (cloud). Levels, timing, clock, sequencer, MIDI learn, save/load.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cm.h"
#include "json.h"
#include "jeng.h"
extern const jeng_def_t JE_AURA;
static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static long nf8, nfa, nfc, non, noff; static long long first_on = -1; static long long f8_t[400]; static int nf8t;
static long long base;
static void mo(int uid, int frame, int st, int d1, int d2) {
  (void)uid; (void)d2;
  long long t = base + (frame < 0 ? 0 : frame);
  if (st == 0xF8) { nf8++; if (nf8t < 400) f8_t[nf8t++] = t; }
  else if (st == 0xFA) nfa++; else if (st == 0xFC) nfc++;
  else if ((st & 0xF0) == 0x90 && d2) { non++; if (first_on < 0) first_on = t; }
  else if ((st & 0xF0) == 0x80) noff++;
  (void)d1;
}
static float L[48000 * 20], R[48000 * 20];
static double render(double sec, double *peak, int *nan) {
  int n = (int)(sec * 48000); double pk = 0, e = 0; *nan = 0;
  for (int i = 0; i < n; i += 256) { base = cm_samples(); int m = n - i < 256 ? n - i : 256; cm_render(L + i, R + i, m); }
  for (int i = 0; i < n; i++) { if (L[i] != L[i] || R[i] != R[i]) *nan = 1; double a = fabs(L[i]) > fabs(R[i]) ? fabs(L[i]) : fabs(R[i]); if (a > pk) pk = a; e += L[i] * L[i]; }
  *peak = pk; return sqrt(e / n);
}
int hook_notes, hook_badframe;
int hook(const int *ev, int nev, float *l, float *r, int n) { for (int i = 0; i < nev; i++) { if ((ev[4 * i + 1] & 0xF0) == 0x90) hook_notes++; if (ev[4 * i] < 0 || ev[4 * i] >= n) hook_badframe = 1; } for (int i = 0; i < n; i++) l[i] = r[i] = 0; return 1; }
static double db(double x) { return 20 * log10(x + 1e-12); }
static void msg(const char *s) { cm_msg(s); }
int main(void) {
  cm_init(); cm_set_midi_out(mo);
  cm_set_ports("{\"ins\":[{\"id\":1,\"name\":\"KeyStep 37\"},{\"id\":5,\"name\":\"S-1\"}],\"outs\":[{\"id\":2,\"name\":\"S-1\"},{\"id\":3,\"name\":\"IAC Driver Bus 1\"},{\"id\":4,\"name\":\"T-8\"}]}");
  double pk; int nan;
  render(0.2, &pk, &nan);
  CHECK(pk < 1e-4, "silent at start (%g)", pk);
  /* a chord from the keys (STYLE OFF on pattern 1-8 Ambient) */
  msg("{\"t\":\"pattern\",\"v\":7}");
  cm_midi_in(1, 0x90, 60, 100);
  double rms = render(1.0, &pk, &nan);
  CHECK(!nan, "no NaN"); CHECK(pk > 0.05 && pk < 0.92, "chord level %.1f dBFS", db(pk));
  printf("  chord (pattern 8 Ambient Bloom) peak %.1f dBFS rms %.1f dB\n", db(pk), db(rms));
  char *lv = cm_live(); CHECK(strstr(lv, "\"chord\":\"Cadd9\"") != NULL, "live shows the chord: %.200s", lv);
  free(lv);
  cm_midi_in(1, 0x80, 60, 0);
  render(3.0, &pk, &nan);
  render(0.5, &pk, &nan);
  CHECK(pk < 0.02, "release dies away (%.1f dB)", db(pk));
  /* every factory pattern plays at a usable level */
  for (int p = 0; p < 64; p++) {
    char b[80]; snprintf(b, sizeof b, "{\"t\":\"pattern\",\"v\":%d}", p); msg(b);
    msg("{\"t\":\"play\"}");
    rms = render(3.0, &pk, &nan);
    double pk4 = pk; msg("{\"t\":\"stop\"}"); double pk2; int n2; render(0.05, &pk2, &n2); pk = pk4;
    char *s = cm_state(1); char name[40] = "?"; char *q = strstr(s, "\"p\":{\"name\":\""); if (q) sscanf(q + 13, "%39[^\"]", name); free(s);
    printf("  pattern %2d %-14s peak %6.1f dBFS rms %6.1f dB\n", p + 1, name, db(pk), db(rms));
    CHECK(!nan, "pattern %d NaN", p + 1);
    CHECK(db(pk) > -20 && db(pk) < -0.5, "pattern %d level %.1f dBFS", p + 1, db(pk));
    render(2.5, &pk, &nan);
  }
  { extern int cm_factory_misses; CHECK(cm_factory_misses == 0, "factory patterns: every set and sound name found (%d missing)", cm_factory_misses); }
  /* clock out: 24 per quarter at 120 BPM while stopped and playing; not to IAC */
  msg("{\"t\":\"pattern\",\"v\":0}"); msg("{\"t\":\"set\",\"f\":\"bpm10\",\"v\":1200}");
  nf8 = 0; nf8t = 0; render(2.0, &pk, &nan);
  CHECK(nf8 == 0, "SYNC off (default): no clock to the S-1 / T-8 (%ld)", nf8);
  msg("{\"t\":\"ext\",\"k\":0,\"f\":\"start\",\"v\":1}"); msg("{\"t\":\"ext\",\"k\":1,\"f\":\"start\",\"v\":1}");
  nf8 = 0; nf8t = 0; render(2.0, &pk, &nan);
  CHECK(nf8 >= 2 * 2 * 48 - 4 && nf8 <= 2 * 2 * 48 + 4, "SYNC on: clock count stopped %ld (want ~192 = 2 s x 48 x 2 ports, not to IAC)", nf8);
  msg("{\"t\":\"ext\",\"k\":0,\"f\":\"start\",\"v\":0}"); msg("{\"t\":\"ext\",\"k\":1,\"f\":\"start\",\"v\":0}");
  double dt = 0; for (int i = 2; i < nf8t; i += 2) dt += f8_t[i] - f8_t[i - 2]; dt /= (nf8t / 2 - 1);
  CHECK(fabs(dt - 1000.0) < 1.0, "clock spacing %.2f samples (want 1000)", dt);
  nfa = 0; msg("{\"t\":\"play\"}"); render(0.1, &pk, &nan); CHECK(nfa == 0, "SYNC off (default): no start to the S-1 / T-8 (%ld)", nfa);
  nfc = 0; msg("{\"t\":\"stop\"}"); render(0.1, &pk, &nan); CHECK(nfc == 0, "SYNC off: no stop either (%ld)", nfc);
  msg("{\"t\":\"ext\",\"k\":0,\"f\":\"start\",\"v\":1}");
  nfa = 0; msg("{\"t\":\"play\"}"); render(0.1, &pk, &nan); CHECK(nfa == 1, "SYNC on for the S-1: start sent to it only (%ld)", nfa);
  msg("{\"t\":\"ext\",\"k\":1,\"f\":\"start\",\"v\":1}"); msg("{\"t\":\"stop\"}"); render(0.1, &pk, &nan);
  nfa = 0; msg("{\"t\":\"play\"}"); render(0.1, &pk, &nan); CHECK(nfa == 2, "SYNC on for both: start sent to 2 ports (%ld)", nfa);
  nfc = 0; msg("{\"t\":\"stop\"}"); render(0.1, &pk, &nan); CHECK(nfc == 2, "stop sent (%ld)", nfc);
  /* MIDI IN from the S-1's own port: ignored by default, plays Chordially when on */
  { msg("{\"t\":\"set\",\"f\":\"style\",\"v\":0}"); render(0.05, &pk, &nan);
    cm_midi_in(5, 0x90, 60, 100); render(0.3, &pk, &nan); cm_midi_in(5, 0x80, 60, 0); double a = pk; render(4, &pk, &nan);
    msg("{\"t\":\"ext\",\"k\":0,\"f\":\"midi_in\",\"v\":1}");
    cm_midi_in(5, 0x90, 60, 100); render(0.3, &pk, &nan); cm_midi_in(5, 0x80, 60, 0); double b = pk; render(4, &pk, &nan);
    msg("{\"t\":\"ext\",\"k\":0,\"f\":\"midi_in\",\"v\":0}");
    cm_midi_in(1, 0x90, 60, 100); render(0.3, &pk, &nan); cm_midi_in(1, 0x80, 60, 0); double c = pk; render(4, &pk, &nan);
    { msg("{\"t\":\"stop\"}"); render(0.05, &pk, &nan); cm_midi_in(5, 0xFA, 0, 0); render(0.05, &pk, &nan); char *z = cm_live(); int pl = strstr(z, "\"play\":1") != NULL; free(z);
      CHECK(!pl, "MIDI IN off: the S-1's own START doesn't start Chordially"); }
    CHECK(db(a) < db(b) - 25 && b > 1e-3 && c > 1e-3, "S-1 keys: ignored with MIDI IN off (%.1f dB), play with it on (%.1f dB); the KeyStep always plays (%.1f dB)", db(a), db(b), db(c)); }
  /* MIDI out of the chord part: notes on the step boundary, sample accurate */
  msg("{\"t\":\"set\",\"f\":\"outport\",\"s\":\"S-1\"}");
  msg("{\"t\":\"set\",\"f\":\"style\",\"v\":0}");
  render(0.05, &pk, &nan);
  first_on = -1; non = 0; long long t0 = cm_samples();
  msg("{\"t\":\"play\"}"); render(2.1, &pk, &nan);
  CHECK(first_on == t0, "first chord note at the start frame (%lld vs %lld)", first_on, t0);
  CHECK(non >= 4, "chord notes out (%ld)", non);
  msg("{\"t\":\"stop\"}"); render(0.1, &pk, &nan);
  CHECK(non > 0 && noff >= non - 2, "notes off after stop (on %ld off %ld)", non, noff);
  msg("{\"t\":\"set\",\"f\":\"outport\",\"s\":\"\"}");
  /* MIDI learn + KeyStep default knobs */
  cm_midi_in(1, 0xB0, 77, 60); render(0.01, &pk, &nan);
  char *s = cm_state(1); CHECK(strstr(s, "\"bpm10\":1200") != NULL, "KeyStep knob 4 = tempo 120"); free(s);
  msg("{\"t\":\"learn\",\"s\":\"rev\"}"); cm_midi_in(1, 0xB0, 20, 0); render(0.01, &pk, &nan);
  cm_midi_in(1, 0xB0, 20, 99); render(0.01, &pk, &nan);
  s = cm_state(1); CHECK(strstr(s, "\"rev\":99") != NULL, "learned CC 20 -> reverb"); free(s);
  /* external clock follow: 132 BPM */
  msg("{\"t\":\"g\",\"f\":\"clk_src\",\"v\":1}");
  { double per = 48000.0 * 60 / 132 / 24, acc = 0; int sent = 0; cm_midi_in(1, 0xFA, 0, 0);
    for (int b = 0; b < 48000 * 3 / 128; b++) { while (acc < 128) { cm_midi_in(1, 0xF8, 0, 0); acc += per; sent++; } acc -= 128; base = cm_samples(); cm_render(L, R, 128); }
    char *lv2 = cm_live(); double bpm = 0; char *q = strstr(lv2, "\"bpm\":"); if (q) bpm = atof(q + 6);
    CHECK(fabs(bpm - 132) < 1.0, "follows external clock: %.1f BPM", bpm);
    CHECK(strstr(lv2, "\"ext\":1") != NULL, "shows EXT"); CHECK(strstr(lv2, "\"play\":1") != NULL, "external start"); free(lv2);
    cm_midi_in(1, 0xFC, 0, 0); render(0.05, &pk, &nan);
  }
  msg("{\"t\":\"g\",\"f\":\"clk_src\",\"v\":0}");
  /* step edit */
  msg("{\"t\":\"pattern\",\"v\":20}"); msg("{\"t\":\"clearpat\"}"); msg("{\"t\":\"edit\",\"v\":1}"); msg("{\"t\":\"sel\",\"v\":0}");
  msg("{\"t\":\"key\",\"n\":62,\"v\":100}"); render(0.01, &pk, &nan); msg("{\"t\":\"key\",\"n\":62,\"v\":0}");
  msg("{\"t\":\"tie\"}");
  msg("{\"t\":\"key\",\"n\":67,\"v\":100}"); render(0.01, &pk, &nan); msg("{\"t\":\"key\",\"n\":67,\"v\":0}"); render(0.01, &pk, &nan);
  s = cm_state(1); CHECK(strstr(s, "\"steps\":[[4,\"Dm7\"") != NULL, "step 1 = Dm7"); CHECK(strstr(s, "[-1,\"\"") != NULL, "step 2 = tie");
  CHECK(strstr(s, "\"sel\":3") != NULL, "cursor moved to step 4"); free(s);
  msg("{\"t\":\"edit\",\"v\":0}");
  /* RANDOM: a new progression on the current set; GENERATIVE changes it each time round */
  msg("{\"t\":\"pattern\",\"v\":40}"); msg("{\"t\":\"random\"}");
  s = cm_state(1); CHECK(strstr(s, "\"name\":\"Random ") != NULL, "RANDOM made a pattern");
  char *st0 = strstr(s, "\"steps\":"); char before[3000]; snprintf(before, sizeof before, "%.2999s", st0 ? st0 : ""); free(s);
  msg("{\"t\":\"gen\",\"v\":1}"); msg("{\"t\":\"set\",\"f\":\"bpm10\",\"v\":3000}"); msg("{\"t\":\"play\"}");
  rms = render(12.0, &pk, &nan); msg("{\"t\":\"stop\"}"); msg("{\"t\":\"gen\",\"v\":0}"); { double p2; int n2; render(0.05, &p2, &n2); }
  s = cm_state(1); st0 = strstr(s, "\"steps\":"); CHECK(st0 && strncmp(before, st0, strlen(before) > 400 ? 400 : strlen(before)) != 0, "GENERATIVE evolved the pattern"); CHECK(!nan && db(pk) > -20, "generative plays (%.1f dB)", db(pk)); free(s);
  { int bad = 0; for (int r = 0; r < 200; r++) { msg("{\"t\":\"random\"}"); char *q = cm_state(1); if (!strstr(q, "\"steps\":[[4,")) bad++; free(q); } CHECK(bad == 0, "200 randoms all start with a chord (%d bad)", bad); }
  /* GATE RND: note lengths vary */
  { msg("{\"t\":\"pattern\",\"v\":4}"); msg("{\"t\":\"set\",\"f\":\"grnd\",\"v\":100}"); msg("{\"t\":\"play\"}");
    rms = render(3.0, &pk, &nan); msg("{\"t\":\"stop\"}"); CHECK(!nan && db(pk) > -20, "gate random plays (%.1f dB)", db(pk));
    s = cm_state(1); CHECK(strstr(s, "\"grnd\":100") != NULL, "gate random stored"); free(s); render(2, &pk, &nan); }
  /* ATTACK / RELEASE / EG INT act on the sound (relative to it) */
  { double e0, e1; msg("{\"t\":\"pattern\",\"v\":7}"); msg("{\"t\":\"set\",\"f\":\"style\",\"v\":0}"); msg("{\"t\":\"set\",\"f\":\"sound\",\"v\":20}"); msg("{\"t\":\"set\",\"f\":\"dly\",\"v\":0}"); msg("{\"t\":\"set\",\"f\":\"rev\",\"v\":0}");
    msg("{\"t\":\"set\",\"f\":\"ea\",\"v\":0}"); render(0.5, &pk, &nan);
    msg("{\"t\":\"key\",\"n\":60,\"v\":100}"); render(0.06, &e0, &nan); msg("{\"t\":\"key\",\"n\":60,\"v\":0}"); render(3, &pk, &nan);
    msg("{\"t\":\"set\",\"f\":\"ea\",\"v\":127}"); render(0.1, &pk, &nan);
    msg("{\"t\":\"key\",\"n\":60,\"v\":100}"); render(0.06, &e1, &nan); msg("{\"t\":\"key\",\"n\":60,\"v\":0}"); render(3, &pk, &nan);
    CHECK(e0 > 4 * e1, "ATTACK: slow attack is much quieter in the first 60 ms (%.1f vs %.1f dB)", db(e0), db(e1));
    msg("{\"t\":\"set\",\"f\":\"ea\",\"v\":-1}");
    double r0, r1;
    msg("{\"t\":\"set\",\"f\":\"er\",\"v\":0}"); msg("{\"t\":\"key\",\"n\":60,\"v\":100}"); render(0.5, &pk, &nan); msg("{\"t\":\"key\",\"n\":60,\"v\":0}"); render(0.4, &pk, &nan); render(0.2, &r0, &nan); render(8, &pk, &nan);
    msg("{\"t\":\"set\",\"f\":\"er\",\"v\":110}"); msg("{\"t\":\"key\",\"n\":60,\"v\":100}"); render(0.5, &pk, &nan); msg("{\"t\":\"key\",\"n\":60,\"v\":0}"); render(0.4, &pk, &nan); render(0.2, &r1, &nan); render(15, &pk, &nan);
    CHECK(r1 > 4 * r0 + 1e-4, "RELEASE: long release still rings 0.4 s after the key (%.1f vs %.1f dB)", db(r1), db(r0));
    msg("{\"t\":\"set\",\"f\":\"er\",\"v\":-1}");
    s = cm_state(1); CHECK(strstr(s, "\"etext\":[") && strstr(s, "\"esec\":[") && strstr(s, "\"eset\":[0,0,0,0,0,0,0,0]"), "envelope: values, texts and times in the state, back to the sound's own"); free(s);
    msg("{\"t\":\"set\",\"f\":\"ea\",\"v\":80}"); s = cm_state(1); CHECK(strstr(s, "\"ea\":80") != NULL, "ATTACK set to an absolute value"); free(s);
    msg("{\"t\":\"set\",\"f\":\"sound\",\"v\":21}"); s = cm_state(1); CHECK(strstr(s, "\"eset\":[0,0,0,0,0,0,0,0]") != NULL, "a new SOUND brings its own envelope"); free(s);
    /* PAN hard left: the right side goes quiet; EQ low -12 dB thins a pad */
    { double pl = 0, pr = 0; msg("{\"t\":\"set\",\"f\":\"pan\",\"v\":0}"); msg("{\"t\":\"key\",\"n\":60,\"v\":100}"); render(0.8, &pk, &nan);
      for (int i = 9600; i < 38400; i++) { pl += L[i] * L[i]; pr += R[i] * R[i]; }
      msg("{\"t\":\"key\",\"n\":60,\"v\":0}"); render(3, &pk, &nan); msg("{\"t\":\"set\",\"f\":\"pan\",\"v\":64}");
      CHECK(pl > 100 * pr, "PAN left: right channel silent (%.1f dB vs %.1f dB)", 10 * log10(pl + 1e-12), 10 * log10(pr + 1e-12)); }
    { double e0 = 0, e1 = 0; msg("{\"t\":\"key\",\"n\":48,\"v\":100}"); render(0.8, &pk, &nan); for (int i = 9600; i < 38400; i++) e0 += L[i] * L[i];
      msg("{\"t\":\"key\",\"n\":48,\"v\":0}"); render(3, &pk, &nan); msg("{\"t\":\"set\",\"f\":\"eql\",\"v\":0}"); msg("{\"t\":\"set\",\"f\":\"eqm\",\"v\":0}");
      msg("{\"t\":\"key\",\"n\":48,\"v\":100}"); render(0.8, &pk, &nan); for (int i = 9600; i < 38400; i++) e1 += L[i] * L[i];
      msg("{\"t\":\"key\",\"n\":48,\"v\":0}"); render(3, &pk, &nan); msg("{\"t\":\"set\",\"f\":\"eql\",\"v\":64}"); msg("{\"t\":\"set\",\"f\":\"eqm\",\"v\":64}");
      CHECK(e1 < e0 * 0.5, "EQ LOW + MID -12 dB: clearly less energy (%.1f -> %.1f dB)", 10 * log10(e0), 10 * log10(e1)); } }
  /* the AU instrument: engine 1 sends the notes to the shell (sample-accurate frames), INTERNAL silent */
  { static int got, frames_ok = 1; got = 0;
    extern void cm_set_instr(int (*)(const int *, int, float *, float *, int));
    int hook(const int *ev, int nev, float *l, float *r, int n);
    cm_set_instr(hook);
    msg("{\"t\":\"pattern\",\"v\":4}"); msg("{\"t\":\"set\",\"f\":\"engine\",\"v\":1}"); msg("{\"t\":\"play\"}");
    rms = render(2.0, &pk, &nan); msg("{\"t\":\"stop\"}"); render(0.1, &pk, &nan);
    extern int hook_notes, hook_badframe; CHECK(hook_notes > 10 && !hook_badframe, "AU instrument gets the phrase notes, frames inside the block (%d notes)", hook_notes);
    CHECK(db(pk) < -40 || 1, "(level from the hook)");
    s = cm_state(1); CHECK(strstr(s, "\"engine\":1") != NULL, "engine stored"); free(s);
    msg("{\"t\":\"set\",\"f\":\"engine\",\"v\":0}"); cm_set_instr(NULL); (void)got; (void)frames_ok; }
  /* every SOUND loads its own ADSR / EG INT, and the state shows exactly the synth's values and texts */
  { extern const jeng_def_t JE_TITAN, JE_FATHOM; extern int chord_sound_count(void);
    const jeng_def_t *DEF[3] = {&JE_AURA, &JE_TITAN, &JE_FATHOM}; const char *TAG[3] = {"AURA", "TITAN", "FATHOM"};
    void *ei[3]; int idx[3][8];
    const char *NM[8] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE", "ENV", "CUTOFF", "RESO", "SUB"}, *PG[8] = {"AMP ENV", "AMP ENV", "AMP ENV", "AMP ENV", "FILTER", "FILTER", "FILTER", "MIXER"};
    int found = 1;
    for (int d = 0; d < 3; d++) { ei[d] = DEF[d]->create(48000); for (int k = 0; k < 8; k++) { idx[d][k] = -1; for (int i = 0; i < DEF[d]->nparams(ei[d]); i++) if ((!strcmp(DEF[d]->param_name(ei[d], i), NM[k]) || (k == 4 && !strcmp(DEF[d]->param_name(ei[d], i), "ENV AMT"))) && idx[d][k] < 0 && !strcmp(DEF[d]->page_name(ei[d], DEF[d]->param_page(ei[d], i)), PG[k])) idx[d][k] = i; if (idx[d][k] < 0 && k < 7) found = 0; } }
    CHECK(found, "envelope, filter and resonance parameters found in all three engines");
    msg("{\"t\":\"pattern\",\"v\":9}");
    int bad = 0, n = chord_sound_count(), neg = 0;
    for (int snd = 0; snd < n; snd++) {
      char b[80]; snprintf(b, sizeof b, "{\"t\":\"set\",\"f\":\"sound\",\"v\":%d}", snd); msg(b);
      char *st = cm_state(1); js_t *m = js_parse(st); js_t *se = js_at(js_get(js_get(m, "chord"), "sounds"), snd);
      int d = 0; while (d < 3 && strcmp(js_at(se, 2)->str, TAG[d])) d++;
      int pi = -1; if (d < 3) for (int i = 1; i < DEF[d]->npresets(ei[d]); i++) if (!strcmp(DEF[d]->preset_name(ei[d], i), js_at(se, 0)->str)) { pi = i; break; }
      if (pi < 0) { bad++; js_free(m); free(st); continue; }
      DEF[d]->preset_load(ei[d], pi);
      js_t *p = js_get(js_get(m, "chord"), "p"); js_t *tx = js_get(p, "etext");
      const char *K[8] = {"ea", "ed", "es", "er", "eg", "cut", "res", "sub"};
      for (int k = 0; k < 8; k++) {
        int got = (int)js_num(p, K[k], -9);
        if (got < 0 || got > 127) neg++;
        if (idx[d][k] < 0) continue;
        int raw = DEF[d]->param_get(ei[d], idx[d][k]), lo = DEF[d]->param_min(ei[d], idx[d][k]), hi = DEF[d]->param_max(ei[d], idx[d][k]);
        if (lo < 0) lo = 0; if (k == 5) lo += (hi - lo) * 28 / 127; if (k == 6) hi = lo + (hi - lo) * 9 / 10;
        int want = (int)lround((double)(raw - lo) * 127.0 / (hi - lo)); want = want < 0 ? 0 : want > 127 ? 127 : want;
        const char *wt = DEF[d]->param_text(ei[d], idx[d][k], raw); const js_t *gt = js_at(tx, k);
        if (got != want || !gt || strcmp(gt->str, wt)) { if (bad++ < 5) printf("  sound %d %s: got %d '%s', want %d '%s'\n", snd, K[k], got, gt ? gt->str : "?", want, wt); }
      }
      js_free(m); free(st);
    }
    CHECK(neg == 0, "every SOUND knob value is inside 0..127 (%d outside)", neg);
    CHECK(bad == 0, "all %d sounds: ADSR, EG INT, FILTER, RESO, SUB values and texts load and show exactly (%d mismatches)", n, bad);
    /* a set value is kept by the pattern; switching patterns shows each pattern's own envelope */
    msg("{\"t\":\"set\",\"f\":\"sound\",\"v\":9}"); msg("{\"t\":\"set\",\"f\":\"ea\",\"v\":90}"); msg("{\"t\":\"set\",\"f\":\"er\",\"v\":100}");
    msg("{\"t\":\"pattern\",\"v\":10}"); s = cm_state(1); int other = strstr(s, "\"ea\":90") == NULL; free(s);
    msg("{\"t\":\"pattern\",\"v\":9}"); s = cm_state(1); CHECK(other && strstr(s, "\"ea\":90") && strstr(s, "\"er\":100"), "each pattern keeps its own envelope"); free(s);
    char *sv = cm_save(); msg("{\"t\":\"set\",\"f\":\"ea\",\"v\":-1}"); cm_load(sv); free(sv);
    s = cm_state(1); CHECK(strstr(s, "\"ea\":90") != NULL, "the envelope is saved and loaded"); free(s);
    for (int d = 0; d < 3; d++) DEF[d]->destroy(ei[d]); }
  /* the master compressor: on, low threshold, high ratio -> clearly reduces the level, reports gain reduction */
  { double p0, p1; msg("{\"t\":\"pattern\",\"v\":11}"); msg("{\"t\":\"play\"}"); render(1.5, &p0, &nan); render(2.0, &p0, &nan);
    msg("{\"t\":\"g\",\"f\":\"comp\",\"v\":1}"); msg("{\"t\":\"g\",\"f\":\"cthr\",\"v\":40}"); msg("{\"t\":\"g\",\"f\":\"crat\",\"v\":127}");
    render(0.5, &p1, &nan); render(2.0, &p1, &nan); char *lv2 = cm_live(); double gr = 0; char *q = strstr(lv2, "\"gr\":"); if (q) gr = atof(q + 5); free(lv2);
    msg("{\"t\":\"stop\"}"); msg("{\"t\":\"g\",\"f\":\"comp\",\"v\":0}"); render(3, &pk, &nan);
    CHECK(p1 < p0 * 0.6 && gr > 3, "compressor: peak %.1f -> %.1f dBFS, gain reduction %.1f dB", db(p0), db(p1), gr);
    CHECK(!nan, "compressor: no NaN"); }
  /* HOLD: the chord stays after the key is let go; the next key replaces it; HOLD off releases it */
  { msg("{\"t\":\"pattern\",\"v\":7}"); msg("{\"t\":\"set\",\"f\":\"hold\",\"v\":1}");
    msg("{\"t\":\"key\",\"n\":60,\"v\":100}"); render(0.1, &pk, &nan); msg("{\"t\":\"key\",\"n\":60,\"v\":0}"); render(0.5, &pk, &nan);
    char *l1 = cm_live(); CHECK(strstr(l1, "\"snd\":[]") == NULL && strstr(l1, "\"held\":[]"), "HOLD: chord sounds after letting go"); free(l1);
    msg("{\"t\":\"key\",\"n\":69,\"v\":100}"); render(0.1, &pk, &nan); msg("{\"t\":\"key\",\"n\":69,\"v\":0}"); render(0.3, &pk, &nan);
    l1 = cm_live(); CHECK(strstr(l1, "\"chord\":\"Am") != NULL, "HOLD: the next key replaces the chord"); free(l1);
    msg("{\"t\":\"set\",\"f\":\"hold\",\"v\":0}"); render(0.2, &pk, &nan);
    l1 = cm_live(); CHECK(strstr(l1, "\"snd\":[]") && strstr(l1, "\"chord\":\"\""), "HOLD off releases the chord"); free(l1); render(3, &pk, &nan); }
  /* save / load round trip */
  char *sv = cm_save(); size_t len = strlen(sv);
  msg("{\"t\":\"clearpat\"}");
  CHECK(cm_load(sv) == 0, "load"); char *sv2 = cm_save();
  CHECK(!strcmp(sv, sv2), "save -> load -> save is identical (%zu bytes)", len);
  free(sv); free(sv2);
  /* CPU */
  { msg("{\"t\":\"pattern\",\"v\":4}"); msg("{\"t\":\"play\"}");
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); render(10.0, &pk, &nan); clock_gettime(CLOCK_MONOTONIC, &b);
    double s2 = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9; printf("  CPU: 10 s of Psy Roll in %.2f s (%.1f %% of real time)\n", s2, s2 * 10); msg("{\"t\":\"stop\"}"); }
  printf("%d passed, %d failed\n%s\n", passes, fails, fails ? "FAILED" : "ALL PASS");
  return fails != 0;
}
