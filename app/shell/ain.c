/* Strata Native, step 3.6: audio in from devices, as EXTERNAL's tracks (ain.h). GPL-3.0-only.
 * A device that feeds a track runs its own input unit (AUHAL, input only) at its own rate; its callback writes into an
 * aring_t; Strata's audio thread reads each routed track's channels once a block, resampled to 44100 Hz with the
 * drift between the two clocks corrected (the buffer's fill held at a target a little above the device's chunk). */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ain.h"
#include "shell.h"

/* ------------------------------------------------------------------ the buffer */
int aring_init(aring_t *a, int ch, double rate)
{
    memset(a, 0, sizeof *a);
    a->ch = ch < 1 ? 1 : ch;
    a->cap = 16384;                                      /* (0.34 s at 48 kHz: far above any target) */
    a->buf = calloc((size_t)a->cap * a->ch, sizeof(float));
    a->base = rate / 48000.0;
    a->target = 512;
    return a->buf ? 0 : -1;
}
void aring_free(aring_t *a) { free(a->buf); a->buf = NULL; }
void aring_write(aring_t *a, const float *f, uint32_t n)
{
    uint64_t w = a->w;
    for (uint32_t i = 0; i < n; i++) memcpy(a->buf + (size_t)((w + i) & (a->cap - 1)) * a->ch, f + (size_t)i * a->ch, sizeof(float) * a->ch);
    if (n > a->chunk_max) a->chunk_max = n;
    __atomic_store_n(&a->w, w + n, __ATOMIC_RELEASE);
}
static inline float at(const aring_t *a, uint64_t i, int c) { return a->buf[(size_t)(i & (a->cap - 1)) * a->ch + c]; }
void aring_read(aring_t *a, int n, float *out)
{
    uint64_t w = __atomic_load_n(&a->w, __ATOMIC_ACQUIRE);
    double t = a->chunk_max * 2.0 + 64;                  /* the target: two chunks (+ the interpolator's frames) */
    if (t < 256) t = 256;
    if (t > a->cap / 4) t = a->cap / 4;
    if (t > a->target) a->target = t;                    /* (it only grows: a smaller chunk later is no reason to shrink) */
    if (!a->primed) {
        if ((double)w < a->target + 8) { memset(out, 0, sizeof(float) * n * a->ch); return; }
        a->r = (double)w - a->target; a->fill_lp = a->target; a->integ = 0; a->adj = 0; a->primed = 1;
    }
    double fill = (double)w - a->r;
    if (fill < 4 || fill > a->cap - 8) {                 /* the device stopped, stalled or raced: start again cleanly */
        if (fill < 4) a->under++; else a->over++;
        a->resets++; a->primed = 0;
        memset(out, 0, sizeof(float) * n * a->ch);
        return;
    }
    /* drift: the fill (smoothed over the device's chunking) held at the target. Proportional (time constant about 2
       s) plus a much slower integral (about 2 min) that takes over a steady drift, so the fill settles exactly on
       the target; slow on purpose: no audible pitch movement (at most 0.6 %, a steady drift is 0.01-0.1 %) */
    a->fill_lp += (fill - a->fill_lp) * 0.01;
    double e = (a->fill_lp - a->target) / a->target;
    a->integ += e * 2e-7;
    if (a->integ > 0.006) a->integ = 0.006; else if (a->integ < -0.006) a->integ = -0.006;
    a->adj = e * 1e-2 + a->integ;
    if (a->adj > 0.006) a->adj = 0.006; else if (a->adj < -0.006) a->adj = -0.006;
    double step = a->base * (1 + a->adj);
    for (int i = 0; i < n; i++) {
        double rr = a->r; uint64_t k = (uint64_t)rr; double x = rr - (double)k;
        for (int c = 0; c < a->ch; c++) {                /* 4-point Hermite between frames k and k+1 */
            float y0 = at(a, k - 1, c), y1 = at(a, k, c), y2 = at(a, k + 1, c), y3 = at(a, k + 2, c);
            double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
            out[i * a->ch + c] = (float)(((c3 * x + c2) * x + c1) * x + y1);
        }
        a->r += step;
    }
}
double aring_fill_ms(const aring_t *a) { return a->primed ? ((double)a->w - a->r) / (a->base * 48.0) : 0; }

/* ------------------------------------------------------------------ the routes and the streams */
#define NS 4                                             /* at most one stream per track */
typedef struct {
    char uid[160], name[128];
    int ch; double rate;
    aring_t ring;
    float *blk; int blk_seq;                             /* this block's frames (all channels), read once per block */
    void *unit;                                          /* the device's input unit (macOS) / the test thread (Linux) */
    volatile int live;
} stream_t;
typedef struct { int s, ch, ch2; } route_t;              /* stream index (-1 none), channels */
static stream_t S_[NS];
static route_t R_[AIN_N];
static void routes_init(void) { static int done; if (!done) { done = 1; for (int k = 0; k < AIN_N; k++) R_[k].s = -1; } }
static pthread_mutex_t rm = PTHREAD_MUTEX_INITIALIZER;   /* the main thread rewiring; the audio thread only trylocks */
static int seq;

static int dev_open(stream_t *s);                        /* (platform) */
static void dev_close(stream_t *s);

static int find_dev(const char *uid, adev_t *d)
{
    adev_t list[64]; int n = ain_devices(list, 64);
    for (int i = 0; i < n; i++) if (!strcmp(list[i].uid, uid) && list[i].in_ch > 0) { *d = list[i]; return 0; }
    return -1;
}
static void drop_unused(void)
{
    for (int i = 0; i < NS; i++) if (S_[i].live) {
        int used = 0;
        for (int k = 0; k < AIN_N; k++) if (R_[k].s == i) used = 1;
        if (!used) { dev_close(&S_[i]); aring_free(&S_[i].ring); free(S_[i].blk); S_[i].blk = NULL; S_[i].live = 0; sh_log("audio in: %s closed", S_[i].name); }
    }
}
int ain_route(int k, const char *uid, int ch)
{
    if (k < 0 || k >= AIN_N) return -1;
    pthread_mutex_lock(&rm); routes_init();
    R_[k].s = -1;
    int rc = 0;
    if (uid && *uid) {
        int si = -1;
        for (int i = 0; i < NS; i++) if (S_[i].live && !strcmp(S_[i].uid, uid)) si = i;
        if (si < 0) {
            adev_t d;
            if (find_dev(uid, &d)) { sh_log("audio in: track %d: device %s is not there", k + 1, uid); rc = -1; goto out; }
            for (int i = 0; i < NS && si < 0; i++) if (!S_[i].live) si = i;
            if (si < 0) { rc = -1; goto out; }
            stream_t *s = &S_[si];
            memset(s, 0, sizeof *s);
            snprintf(s->uid, sizeof s->uid, "%s", d.uid); snprintf(s->name, sizeof s->name, "%s", d.name);
            s->ch = d.in_ch; s->rate = d.rate > 0 ? d.rate : 48000;
            aring_init(&s->ring, s->ch, s->rate);
            s->blk = calloc((size_t)8192 * s->ch, sizeof(float));
            if (dev_open(s)) { aring_free(&s->ring); free(s->blk); s->blk = NULL; sh_log("audio in: %s could not be opened", d.name); rc = -1; goto out; }
            s->live = 1;
            sh_log("audio in: %s opened: %d channels at %.0f Hz", s->name, s->ch, s->rate);
        }
        int c = ch < 0 ? 0 : ch >= S_[si].ch ? S_[si].ch - 1 : ch;
        R_[k].s = si; R_[k].ch = c; R_[k].ch2 = c + 1 < S_[si].ch ? c + 1 : c;
        sh_log("audio in: track %d <- %s, input %d%s", k + 1, S_[si].name, c + 1, R_[k].ch2 != c ? "-2" : " (mono)");
    }
out:
    drop_unused();
    pthread_mutex_unlock(&rm);
    return rc;
}
void ain_begin(void) { seq++; }
int ain_read(int k, int n, float *L, float *R)
{
    if (k < 0 || k >= AIN_N || n > 8192) return 0;
    if (pthread_mutex_trylock(&rm) != 0) return 0;      /* (being rewired: one block without input) */
    routes_init();
    int ok = 0;
    if (R_[k].s >= 0) {
        stream_t *s = &S_[R_[k].s];
        if (s->live) {
            if (s->blk_seq != seq) { aring_read(&s->ring, n, s->blk); s->blk_seq = seq; }
            for (int i = 0; i < n; i++) { L[i] = s->blk[i * s->ch + R_[k].ch]; R[i] = s->blk[i * s->ch + R_[k].ch2]; }
            ok = 1;
        }
    }
    pthread_mutex_unlock(&rm);
    return ok;
}
void ain_tick(char *line, int max)
{
    int n = 0; line[0] = 0;
    pthread_mutex_lock(&rm);
    for (int i = 0; i < NS; i++) if (S_[i].live) {
        aring_t *a = &S_[i].ring;
        n += snprintf(line + n, max - n, "%s%s: fill %.1f ms (target %.1f), drift %+.0f ppm, resets %ld",
                      n ? "; " : "", S_[i].name, aring_fill_ms(a), a->target / (a->base * 48.0), a->adj * 1e6, a->resets);
        if (n >= max) break;
    }
    pthread_mutex_unlock(&rm);
}
int ain_device_present(const char *uid, int want_out)
{
    adev_t l[64]; int n = ain_devices(l, 64);
    for (int i = 0; i < n; i++) if (!strcmp(l[i].uid, uid)) return want_out ? l[i].out_ch > 0 : l[i].in_ch > 0;
    return 0;
}
void ain_stop_all(void)
{
    pthread_mutex_lock(&rm);
    for (int k = 0; k < 4; k++) R_[k].s = -1;
    drop_unused();
    pthread_mutex_unlock(&rm);
}

#ifdef __APPLE__
/* ------------------------------------------------------------------ macOS: CoreAudio */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
static volatile int devs_changed = 1;
static OSStatus devs_cb(AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a, void *c) { (void)o; (void)n; (void)a; (void)c; devs_changed = 1; return noErr; }
int ain_changed(void)
{
    static int listening;
    if (!listening) {
        AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, devs_cb, NULL);
        listening = 1;
    }
    int c = devs_changed; devs_changed = 0; return c;
}
static int chans(AudioDeviceID d, AudioObjectPropertyScope sc)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, sc, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) || !sz) return 0;
    AudioBufferList *l = malloc(sz); int n = 0;
    if (!AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, l)) for (UInt32 i = 0; i < l->mNumberBuffers; i++) n += (int)l->mBuffers[i].mNumberChannels;
    free(l); return n;
}
static void cfstr(CFStringRef s, char *out, int max) { out[0] = 0; if (s) { CFStringGetCString(s, out, max, kCFStringEncodingUTF8); CFRelease(s); } }
int ain_devices(adev_t *list, int max)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz)) return 0;
    int nd = (int)(sz / sizeof(AudioDeviceID)), n = 0;
    AudioDeviceID *ids = malloc(sz);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids)) { free(ids); return 0; }
    for (int i = 0; i < nd && n < max; i++) {
        adev_t *d = &list[n]; memset(d, 0, sizeof *d);
        CFStringRef s = NULL; UInt32 z = sizeof s;
        AudioObjectPropertyAddress p = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &p, 0, NULL, &z, &s)) continue;
        cfstr(s, d->uid, sizeof d->uid);
        p.mSelector = kAudioObjectPropertyName; s = NULL; z = sizeof s;
        if (!AudioObjectGetPropertyData(ids[i], &p, 0, NULL, &z, &s)) cfstr(s, d->name, sizeof d->name);
        Float64 r = 0; z = sizeof r; p.mSelector = kAudioDevicePropertyNominalSampleRate;
        AudioObjectGetPropertyData(ids[i], &p, 0, NULL, &z, &r); d->rate = r;
        d->in_ch = chans(ids[i], kAudioObjectPropertyScopeInput);
        d->out_ch = chans(ids[i], kAudioObjectPropertyScopeOutput);
        UInt32 tt = 0; z = sizeof tt; p.mSelector = kAudioDevicePropertyTransportType;
        if (!AudioObjectGetPropertyData(ids[i], &p, 0, NULL, &z, &tt)) d->agg = tt == kAudioDeviceTransportTypeAggregate;
        if (d->in_ch || d->out_ch) n++;
    }
    free(ids); return n;
}
static AudioDeviceID dev_id(const char *uid);
unsigned ain_dev_id(const char *uid) { return (unsigned)dev_id(uid); }
static AudioDeviceID dev_id(const char *uid)
{
    CFStringRef u = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioDeviceID id = 0; AudioValueTranslation t = { &u, sizeof u, &id, sizeof id };
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof t;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &t);
    CFRelease(u); return id;
}
typedef struct { AudioUnit au; stream_t *s; float *buf; } inunit_t;
static OSStatus in_cb(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io)
{
    (void)io; inunit_t *u = ref;
    if (n > 8192) return noErr;
    AudioBufferList l; l.mNumberBuffers = 1;
    l.mBuffers[0].mNumberChannels = (UInt32)u->s->ch; l.mBuffers[0].mDataByteSize = n * 4 * (UInt32)u->s->ch; l.mBuffers[0].mData = u->buf;
    if (AudioUnitRender(u->au, fl, ts, bus, n, &l) == noErr) aring_write(&u->s->ring, u->buf, n);
    return noErr;
}
static int dev_open(stream_t *s)
{
    AudioDeviceID id = dev_id(s->uid);
    if (!id) return -1;
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    inunit_t *u = calloc(1, sizeof *u); u->s = s; u->buf = calloc((size_t)8192 * s->ch, sizeof(float));
    OSStatus e = AudioComponentInstanceNew(c, &u->au);
    UInt32 one = 1, zero = 0;
    if (!e) e = AudioUnitSetProperty(u->au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof one);
    if (!e) e = AudioUnitSetProperty(u->au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof zero);
    if (!e) e = AudioUnitSetProperty(u->au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id, sizeof id);
    AudioStreamBasicDescription f = { 0 };                /* the device's own rate (the input side of AUHAL does not convert rates) */
    f.mSampleRate = s->rate; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = (UInt32)s->ch; f.mBitsPerChannel = 32; f.mBytesPerFrame = f.mBytesPerPacket = 4 * (UInt32)s->ch; f.mFramesPerPacket = 1;
    if (!e) e = AudioUnitSetProperty(u->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
    AURenderCallbackStruct cb = { in_cb, u };
    if (!e) e = AudioUnitSetProperty(u->au, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
    if (!e) e = AudioUnitInitialize(u->au);
    if (!e) e = AudioOutputUnitStart(u->au);
    if (e) {
        sh_log("audio in: %s: CoreAudio error %d (macOS may ask for microphone access: allow it in System Settings > Privacy & Security > Microphone)", s->name, (int)e);
        if (u->au) AudioComponentInstanceDispose(u->au);
        free(u->buf); free(u); return -1;
    }
    s->unit = u; return 0;
}
static void dev_close(stream_t *s)
{
    inunit_t *u = s->unit; if (!u) return;
    AudioOutputUnitStop(u->au); AudioUnitUninitialize(u->au); AudioComponentInstanceDispose(u->au);
    free(u->buf); free(u); s->unit = NULL;
}
#else
/* ------------------------------------------------------------------ Linux (the cloud tests): a pretend interface whose
 * clock runs 0.1 % fast at 48 kHz, delivering a 220 Hz sine (left) and 330 Hz (right) in jittered chunks */
#include <time.h>
#include <unistd.h>
unsigned ain_dev_id(const char *uid) { return !strcmp(uid, "test-usb") || !strcmp(uid, "test-out"); }
int ain_changed(void) { static int once = 1; int c = once; once = 0; return c; }
int ain_devices(adev_t *list, int max)
{
    if (max < 2) return 0;
    memset(list, 0, 2 * sizeof *list);
    snprintf(list[0].uid, sizeof list[0].uid, "test-usb"); snprintf(list[0].name, sizeof list[0].name, "Test USB Audio");
    list[0].in_ch = 2; list[0].out_ch = 2; list[0].rate = 48000;
    snprintf(list[1].uid, sizeof list[1].uid, "test-out"); snprintf(list[1].name, sizeof list[1].name, "Test Speakers");
    list[1].out_ch = 2; list[1].rate = 48000;
    return 2;
}
typedef struct { pthread_t th; stream_t *s; volatile int run; } fake_t;
static void *fake_run(void *p)
{
    fake_t *f = p; double ph = 0, t0, sent = 0; struct timespec ts; float buf[2 * 1024];
    clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec + ts.tv_nsec / 1e9;
    unsigned rnd = 1;
    while (f->run) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double due = (ts.tv_sec + ts.tv_nsec / 1e9 - t0) * f->s->rate * 1.001;
        while (sent + 480 <= due) {
            for (int i = 0; i < 480; i++) { ph += 1.0 / f->s->rate; buf[2 * i] = (float)(0.4 * sin(2 * M_PI * 220 * ph)); buf[2 * i + 1] = (float)(0.4 * sin(2 * M_PI * 330 * ph)); }
            aring_write(&f->s->ring, buf, 480); sent += 480;
        }
        rnd = rnd * 1103515245u + 12345u;
        usleep(4000 + (rnd >> 16) % 6000);               /* (jitter: 4..10 ms between deliveries) */
    }
    return NULL;
}
static int dev_open(stream_t *s)
{
    if (strcmp(s->uid, "test-usb")) return -1;
    fake_t *f = calloc(1, sizeof *f); f->s = s; f->run = 1;
    pthread_create(&f->th, NULL, fake_run, f); s->unit = f; return 0;
}
static void dev_close(stream_t *s) { fake_t *f = s->unit; if (!f) return; f->run = 0; pthread_join(f->th, NULL); free(f); s->unit = NULL; }
#endif
