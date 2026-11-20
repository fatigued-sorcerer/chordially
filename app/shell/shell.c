// SPDX-License-Identifier: GPL-3.0-only
// Chordially: the shell. CoreAudio output (48 kHz, the core renders), CoreMIDI in / out (every source is listened
// to; out stamped to the audio so clock and notes are sample-aligned with what you hear), USB audio in for the S-1 /
// T-8 parts (ain.c: found by name, re-routed when plugged in), the data file (autosaved), the log.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "shell.h"
#include "ain.h"
#include "../core/cm.h"
#include "../core/json.h"

static FILE *g_logfile;
static char data_path[1024];
static char *last_saved;
static pthread_mutex_t loglk = PTHREAD_MUTEX_INITIALIZER;

void sh_log(const char *fmt, ...)
{
    pthread_mutex_lock(&loglk);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    va_list a; va_start(a, fmt);
    if (g_logfile) { fprintf(g_logfile, "%s ", ts); vfprintf(g_logfile, fmt, a); fputc('\n', g_logfile); fflush(g_logfile); }
    va_end(a);
    pthread_mutex_unlock(&loglk);
}
void sh_page_log(const char *text) { sh_log("page: %s", text); }
void sh_note(const char *text) { sh_log("%s", text); }

/* ------------------------------------------------------------------ the data file */
static char *read_file(const char *p)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f);
    return b;
}
static void save_now(void)
{
    char *s = cm_save();
    if (!s) return;
    if (last_saved && !strcmp(s, last_saved)) { free(s); return; }
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", data_path);
    FILE *f = fopen(tmp, "wb");
    if (f) { fputs(s, f); fclose(f); rename(tmp, data_path); sh_log("store: saved %zu bytes", strlen(s)); }
    else sh_log("store: cannot write %s", tmp);
    free(last_saved); last_saved = s;
}

/* ------------------------------------------------------------------ USB audio in (the EXT parts) */
static char routed[2][200];
static int ain_cb(int slot, int n, float *L, float *R)
{
    if (slot == 0) ain_begin();
    return ain_read(slot, n, L, R);
}
static int ci_has(const char *h, const char *w)
{
    size_t lw = strlen(w); if (!lw) return 0;
    for (const char *p = h; *p; p++) { size_t j = 0; while (j < lw && p[j] && (p[j] | 32) == (w[j] | 32)) j++; if (j == lw) return 1; }
    return 0;
}
static int ci_eq(const char *a, const char *b) { while (*a && *b && (*a | 32) == (*b | 32)) a++, b++; return !*a && !*b; }
static void devices(void)
{
    adev_t l[64]; int n = ain_devices(l, 64);
    {   /* the list for the strips' menus: [[name, inputs, aggregate], ...] */
        jb_t b; jb_init(&b); jb_ch(&b, '['); int c = 0;
        for (int i = 0; i < n; i++) if (l[i].in_ch > 0) { if (c++) jb_ch(&b, ','); jb_ch(&b, '['); jb_str_utf8(&b, l[i].name); jb_ch(&b, ','); jb_int(&b, l[i].in_ch); jb_ch(&b, ','); jb_int(&b, l[i].agg); jb_ch(&b, ']'); }
        jb_ch(&b, ']'); cm_set_adevs(b.b); jb_free(&b);
    }
    for (int k = 0; k < cm_ext_count(); k++) {
        const char *want = cm_ext_dev(k), *pick = cm_ext_adev(k);
        const char *other = cm_ext_dev(k ^ 1);
        int found = -1, best = 0, ch = 0;
        if (pick && *pick) {   /* chosen in the strip: that device, those inputs */
            for (int i = 0; i < n; i++) if (l[i].in_ch > 0 && !strcmp(l[i].name, pick)) { found = i; break; }
            ch = cm_ext_ach(k);
        } else {               /* AUTO: a device named like the box; never an aggregate, never one named for both boxes */
            for (int i = 0; i < n; i++) {
                if (l[i].in_ch <= 0 || l[i].agg || !ci_has(l[i].name, want) || ci_has(l[i].name, other)) continue;
                int sc = ci_eq(l[i].name, want) ? 3 : 1;
                if (sc > best) { best = sc; found = i; }
            }
        }
        if (!cm_ext_audio(k)) found = -1;
        char key[200]; snprintf(key, sizeof key, "%s#%d", found >= 0 ? l[found].uid : "", ch);
        if (strcmp(key, routed[k])) {
            const char *uid = found >= 0 ? l[found].uid : "";
            if (ain_route(k, uid, ch) == 0 || !*uid) { snprintf(routed[k], sizeof routed[k], "%s", key); sh_log("ext %d (%s): %s, inputs %d-%d", k + 1, want, found >= 0 ? l[found].name : (cm_ext_audio(k) ? "not connected" : "AUDIO off"), ch + 1, ch + 2); }
        }
        cm_ext_present(k, found >= 0 && routed[k][0] != '#');
    }
}

/* ------------------------------------------------------------------ the page's queue */
static char *outq; static size_t outn, outcap;
static pthread_mutex_t qlk = PTHREAD_MUTEX_INITIALIZER;
void sh_msg(const char *json) { cm_msg(json); }
void sh_post(const char *json)
{
    pthread_mutex_lock(&qlk);
    size_t n = strlen(json);
    if (outn + n + 2 > outcap) { outcap = (outn + n + 2) * 2; outq = realloc(outq, outcap); }
    memcpy(outq + outn, json, n); outn += n; outq[outn++] = ','; outq[outn] = 0;
    pthread_mutex_unlock(&qlk);
}
void sh_set_insert(int (*fn)(float *L, float *R, int n)) { cm_set_insert(fn); }
void sh_set_instr(int (*fn)(const int *ev, int nev, float *L, float *R, int n)) { cm_set_instr(fn); }
void sh_transport(double *bpm, int *playing, double *beat) { cm_transport(bpm, playing, beat); }
char *sh_drain(void)
{
    char *st = cm_state(0), *lv = cm_live();
    pthread_mutex_lock(&qlk);
    size_t q = outn;
    size_t a = st ? strlen(st) : 0, b = lv ? strlen(lv) : 0;
    char *r = malloc(a + b + q + 8); size_t k = 0;
    r[k++] = '[';
    if (q) { memcpy(r + k, outq, q); k += q; outn = 0; }   /* (each queued message ends with a comma) */
    if (st) { memcpy(r + k, st, a); k += a; r[k++] = ','; }
    memcpy(r + k, lv, b); k += b;
    r[k++] = ']'; r[k] = 0;
    pthread_mutex_unlock(&qlk);
    free(st); free(lv);
    return r;
}

/* ------------------------------------------------------------------ MIDI */
#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include <stddef.h>

static MIDIClientRef client;
static MIDIPortRef in_port, out_port;
static SInt32 connected[128]; static int nconnected;
static mach_timebase_info_data_t tb;
static volatile uint64_t cb_host;          /* mach time when the current callback's first frame is heard */
static volatile long long cb_sample;       /* the core's sample count at that frame */
static double out_latency;
typedef struct { SInt32 uid; MIDIEndpointRef e; } epc_t;
static epc_t EPC[64]; static volatile int NEPC;

static void cfname(MIDIObjectRef o, char *out, size_t n)
{
    CFStringRef s = NULL; out[0] = 0;
    if (MIDIObjectGetStringProperty(o, kMIDIPropertyDisplayName, &s) != noErr || !s)
        if (MIDIObjectGetStringProperty(o, kMIDIPropertyName, &s) != noErr || !s) { snprintf(out, n, "MIDI port"); return; }
    CFStringGetCString(s, out, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(s);
}
static SInt32 uid_of(MIDIObjectRef o) { SInt32 u = 0; MIDIObjectGetIntegerProperty(o, kMIDIPropertyUniqueID, &u); return u; }
static void ports(void)
{
    jb_t b; jb_init(&b);
    jb_raw(&b, "{\"ins\":[");
    ItemCount ns = MIDIGetNumberOfSources(), nd = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < ns; i++) {
        MIDIEndpointRef e = MIDIGetSource(i); char name[256]; cfname(e, name, sizeof name); SInt32 u = uid_of(e);
        if (i) jb_ch(&b, ',');
        jb_raw(&b, "{\"id\":"); jb_int(&b, u); jb_raw(&b, ",\"name\":"); jb_str_utf8(&b, name); jb_ch(&b, '}');
        int have = 0; for (int k = 0; k < nconnected; k++) if (connected[k] == u) have = 1;
        if (!have && nconnected < 128) {
            OSStatus st = MIDIPortConnectSource(in_port, e, (void *)(intptr_t)u);
            if (st == noErr) connected[nconnected++] = u;
            sh_log("midi: listening to \"%s\"%s", name, st == noErr ? "" : " FAILED");
        }
    }
    jb_raw(&b, "],\"outs\":[");
    int ne = 0;
    for (ItemCount i = 0; i < nd; i++) {
        MIDIEndpointRef e = MIDIGetDestination(i); char name[256]; cfname(e, name, sizeof name); SInt32 u = uid_of(e);
        if (ne < 64) { EPC[ne].uid = u; EPC[ne].e = e; ne++; }
        if (i) jb_ch(&b, ',');
        jb_raw(&b, "{\"id\":"); jb_int(&b, u); jb_raw(&b, ",\"name\":"); jb_str_utf8(&b, name); jb_ch(&b, '}');
        sh_log("midi: output \"%s\"", name);
    }
    NEPC = ne;
    jb_raw(&b, "]}");
    cm_set_ports(b.b);
    jb_free(&b);
}
static void notify(const MIDINotification *n, void *ref) { (void)ref; if (n->messageID == kMIDIMsgSetupChanged) ports(); }
static void read_cb(const MIDIPacketList *pl, void *ref, void *src)
{
    (void)ref;
    int uid = (int)(intptr_t)src;
    static int running;
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 k = 0; k < pl->numPackets; k++) {
        UInt16 len; memcpy(&len, (const char *)p + offsetof(MIDIPacket, length), sizeof len);
        const Byte *d = (const Byte *)p + offsetof(MIDIPacket, data);
        int st = running;
        for (int i = 0; i < len;) {
            int b0 = d[i], n;
            if (b0 == 0xF0) { while (i < len && d[i] != 0xF7) i++; i++; continue; }
            if (b0 >= 0xF8) { cm_midi_in(uid, b0, 0, 0); i++; continue; }
            if (b0 & 0x80) { st = b0; n = (b0 & 0xF0) == 0xC0 || (b0 & 0xF0) == 0xD0 || b0 == 0xF1 || b0 == 0xF3 ? 2 : b0 == 0xF6 ? 1 : 3; }
            else { if (!st) { i++; continue; } n = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2; }
            int m[3] = { b0 & 0x80 ? b0 : st, 0, 0 }, c = 1;
            int start = b0 & 0x80 ? i + 1 : i, need = b0 & 0x80 ? n - 1 : n;
            for (int j = 0; j < need && start + j < len; j++) m[c++] = d[start + j];
            i = start + need;
            if (m[0] < 0xF0) cm_midi_in(uid, m[0], m[1], m[2]);
        }
        running = st;
        p = MIDIPacketNext(p);
    }
}
/* the core's MIDI out: frame >= 0 inside a block (audio thread), stamped; < 0 now */
static void midi_send(int uid, int frame, int st, int d1, int d2)
{
    MIDIEndpointRef e = 0;
    int ne = NEPC;
    for (int i = 0; i < ne; i++) if (EPC[i].uid == uid) { e = EPC[i].e; break; }
    if (!e) return;
    MIDITimeStamp ts = 0;
    if (frame >= 0 && cb_host) {
        long long off = cm_block_now() + frame - cb_sample;
        double s = (double)off / 48000.0 + out_latency;
        if (s < 0) s = 0;
        ts = cb_host + (MIDITimeStamp)(s * 1e9 * tb.denom / tb.numer);
    }
    Byte bytes[3] = { (Byte)st, (Byte)d1, (Byte)d2 };
    int n = st >= 0xF8 || st == 0xFA || st == 0xFB || st == 0xFC ? 1 : (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3;
    Byte buf[64]; MIDIPacketList *pl = (MIDIPacketList *)buf;
    MIDIPacket *pk = MIDIPacketListInit(pl);
    pk = MIDIPacketListAdd(pl, sizeof buf, pk, ts, (ByteCount)n, bytes);
    if (pk) MIDISend(out_port, e, pl);
}
int sh_midi_start(void)
{
    mach_timebase_info(&tb);
    cm_set_midi_out(midi_send);
    OSStatus e = MIDIClientCreate(CFSTR("Chordially"), notify, NULL, &client);
    if (e) { sh_log("midi: MIDIClientCreate %d", (int)e); return 1; }
    e = MIDIInputPortCreate(client, CFSTR("Chordially in"), read_cb, NULL, &in_port);
    if (e) { sh_log("midi: MIDIInputPortCreate %d", (int)e); return 1; }
    e = MIDIOutputPortCreate(client, CFSTR("Chordially out"), &out_port);
    if (e) { sh_log("midi: MIDIOutputPortCreate %d", (int)e); return 1; }
    ports();
    return 0;
}

/* ------------------------------------------------------------------ audio out */
static AudioUnit au;
static long cbs, cb_min = 1 << 30, cb_max;
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io)
{
    (void)ref; (void)flags; (void)bus;
    if (ts && (ts->mFlags & kAudioTimeStampHostTimeValid)) { cb_host = ts->mHostTime; cb_sample = cm_samples(); }
    else cb_host = 0;
    float *L = (float *)io->mBuffers[0].mData, *R = io->mNumberBuffers > 1 ? (float *)io->mBuffers[1].mData : NULL;
    static float tmp[8192];
    cm_render(L, R ? R : tmp, (int)n);
    cbs++; if ((long)n < cb_min) cb_min = n; if ((long)n > cb_max) cb_max = n;
    return noErr;
}
int sh_audio_start(void)
{
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c) { sh_log("audio: no output unit"); return 1; }
    OSStatus e = AudioComponentInstanceNew(c, &au);
    if (e) { sh_log("audio: AudioComponentInstanceNew %d", (int)e); return 1; }
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = 48000; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = 4; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
    e = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (e) { sh_log("audio: stream format %d", (int)e); return 1; }
    AURenderCallbackStruct cb = { render_cb, NULL };
    e = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (e) { sh_log("audio: render callback %d", (int)e); return 1; }
    e = AudioUnitInitialize(au);
    if (e) { sh_log("audio: initialize %d", (int)e); return 1; }
    {
        AudioDeviceID dev_id = 0; UInt32 z = sizeof dev_id;
        AudioUnitGetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev_id, &z);
        AudioStreamBasicDescription dev = { 0 }; UInt32 sz = sizeof dev;
        AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &dev, &sz);
        UInt32 lat = 0, safe = 0; z = sizeof lat;
        AudioObjectPropertyAddress a = { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyData(dev_id, &a, 0, NULL, &z, &lat);
        a.mSelector = kAudioDevicePropertySafetyOffset; z = sizeof safe; AudioObjectGetPropertyData(dev_id, &a, 0, NULL, &z, &safe);
        out_latency = dev.mSampleRate > 0 ? (lat + safe) / dev.mSampleRate : 0;
        sh_log("audio: device %.0f Hz, latency %u + safety %u frames: %.1f ms added to MIDI timing", dev.mSampleRate, (unsigned)lat, (unsigned)safe, out_latency * 1000);
    }
    e = AudioOutputUnitStart(au);
    if (e) { sh_log("audio: start %d", (int)e); return 1; }
    sh_log("audio: started (the Mac's default output; Chordially renders 48000 Hz)");
    return 0;
}
void sh_audio_stop(void) { if (au) { AudioOutputUnitStop(au); AudioUnitUninitialize(au); AudioComponentInstanceDispose(au); au = NULL; } }
#else
int sh_midi_start(void) { return 0; }
int sh_audio_start(void) { return 0; }
void sh_audio_stop(void) { }
static long cbs, cb_min, cb_max;
#endif

/* ------------------------------------------------------------------ start, tick, quit */
int sh_init(const char *log_path, const char *data_dir)
{
    g_logfile = fopen(log_path, "a");
    sh_log("== Chordially starting");
    snprintf(data_path, sizeof data_path, "%s/chordially.json", data_dir);
    cm_init();
    cm_set_ain(ain_cb);
    char *s = read_file(data_path);
    if (!s) {   /* (renamed from Chord Master: its file is taken over once) */
        char old[1100]; snprintf(old, sizeof old, "%s/chordmaster.json", data_dir);
        s = read_file(old); if (s) sh_log("store: taking over %s", old);
    }
    int had_file = s != NULL;
    char bk[1200]; snprintf(bk, sizeof bk, "%s/chordially-before-build%d.json", data_dir, CM_BUILD);
    if (s) {
        int r = cm_load(s); sh_log("store: %s %s", r == 0 ? "loaded" : "could NOT read", data_path);
        if (cm_loaded_build < CM_BUILD) { FILE *f = fopen(bk, "wb"); if (f) { fputs(s, f); fclose(f); } }   /* (the old file, as it was) */
        free(s);
    }
    else sh_log("store: no %s yet (factory patterns)", data_path);
    if (had_file && cm_loaded_build < CM_BUILD) {   /* a new build: every pattern back to stock (the old file is kept) */
        cm_factory_patterns();
        sh_log("store: new build %d (file from %d): stock patterns, old patterns kept in %s", CM_BUILD, cm_loaded_build, bk);
    }
    cm_chord_startup();
    last_saved = cm_save();
    if (had_file && cm_loaded_build < CM_BUILD) { free(last_saved); last_saved = NULL; save_now(); }   /* (write the fresh start at once) */
    devices();
    return 0;
}
void sh_tick(void)
{
    static int t;
    t++;
    if (ain_changed() || t % 3 == 0) devices();
    if (cm_save_req) { cm_save_req = 0; free(last_saved); last_saved = NULL; save_now(); }
    else if (t % 5 == 0) save_now();
    { char line[600]; ain_tick(line, sizeof line); if (line[0] && t % 10 == 0) sh_log("audio in: %s", line); }
    if (t <= 5 || t % 60 == 0) { sh_log("audio: %ld callbacks of %ld..%ld frames", cbs, cb_min, cb_max); }
}
void sh_quit(void) { save_now(); ain_stop_all(); sh_audio_stop(); sh_log("quit: saved"); }
