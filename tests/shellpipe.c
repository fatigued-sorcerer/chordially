// Chordially: the real core in real time for the page test. Page messages in on stdin (one JSON per line),
// state / live arrays out on stdout (one line each, every ~16 ms). A pretend S-1 audio-in (sine) on slot 0.
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cm.h"
static double ph;
static int ain(int slot, int n, float *L, float *R) { if (slot) return 0; for (int i = 0; i < n; i++) { ph += 220.0 / 48000; L[i] = R[i] = 0.2f * sinf(6.2831853f * ph); } return 1; }
static void mo(int uid, int f, int st, int d1, int d2) { (void)uid; (void)f; (void)st; (void)d1; (void)d2; }
static void *audio(void *a) { (void)a; float L[256], R[256]; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double t0 = t.tv_sec + t.tv_nsec / 1e9; long long done = 0;
  for (;;) { clock_gettime(CLOCK_MONOTONIC, &t); double now = t.tv_sec + t.tv_nsec / 1e9; while (done < (now - t0) * 48000) { cm_render(L, R, 256); done += 256; } usleep(2000); } return NULL; }
static void *reader(void *a) { (void)a; char *line = NULL; size_t cap = 0; while (getline(&line, &cap, stdin) > 0) { line[strcspn(line, "\n")] = 0; if (!strncmp(line, "MIDI ", 5)) { int a, b, c; if (sscanf(line + 5, "%d %d %d", &a, &b, &c) == 3) cm_midi_in(1, a, b, c); } else if (line[0]) cm_msg(line); } exit(0); }
int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  cm_init(); cm_chord_startup(); cm_set_midi_out(mo); cm_set_ain(ain);
  cm_set_ports("{\"ins\":[{\"id\":1,\"name\":\"KeyStep 37\"}],\"outs\":[{\"id\":2,\"name\":\"S-1\"},{\"id\":3,\"name\":\"T-8\"},{\"id\":4,\"name\":\"IAC Driver Bus 1\"}]}");
  cm_set_adevs("[[\"S-1\",2,0],[\"LIttle S-1 and T-8\",4,1],[\"MacBook Air Microphone\",1,0]]");
  cm_ext_present(0, 1);
  pthread_t a, r; pthread_create(&a, NULL, audio, NULL); pthread_create(&r, NULL, reader, NULL);
  for (;;) { char *s = cm_state(0); if (s) { printf("[%s]\n", s); free(s); } char *l = cm_live(); printf("[%s]\n", l); free(l); usleep(16000); }
}
