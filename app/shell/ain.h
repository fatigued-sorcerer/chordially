/* Strata Native, step 3.6: audio in from devices (as EXTERNAL tracks) and the output device. GPL-3.0-only. */
#ifndef AIN_H
#define AIN_H
#include <stdint.h>

/* ---- the input buffer between a device's clock and Strata's (portable; tested in the cloud) ----
 * The device's callback writes frames (interleaved, its own rate); Strata's audio thread reads 48000 Hz blocks. The
 * reader resamples by rate/48000 and corrects the two clocks' drift by holding the buffer's fill at its target. */
typedef struct {
    float *buf; int ch; uint32_t cap;          /* frames, a power of two */
    volatile uint64_t w;                       /* frames written so far (the writer only) */
    volatile uint32_t chunk_max;               /* the largest write seen (sets the target) */
    double r, base, adj, integ, fill_lp, target;
    int primed;
    long resets, under, over;                  /* (for the log) */
} aring_t;
int aring_init(aring_t *a, int ch, double rate);           /* rate: the device's (Hz) */
void aring_free(aring_t *a);
void aring_write(aring_t *a, const float *frames, uint32_t n); /* the device's thread */
void aring_read(aring_t *a, int n, float *out);            /* Strata's audio thread: n frames x ch, interleaved; zeros until primed */
double aring_fill_ms(const aring_t *a);                    /* (for the log) */

/* ---- devices ---- */
typedef struct { char uid[160], name[128]; int in_ch, out_ch; double rate; int agg; } adev_t;   /* agg: an aggregate device */
int ain_devices(adev_t *list, int max);                    /* the devices there are now (with test devices on Linux) */

/* the input routing: EXTERNAL track k (0..3) takes channels ch, ch+1 (ch+1 = ch for a single channel) of device uid;
   uid "" = none. Main thread. */
enum { AIN_N = 16 };                                       /* audio-in slots (Jammer AUDIO tracks) */
int ain_route(int k, const char *uid, int ch);
/* the audio thread: ain_begin once a block, then ain_read per slot (n frames at 48 kHz); 0 when nothing routed */
void ain_begin(void);
int ain_read(int k, int n, float *L, float *R);
void ain_tick(char *line, int max);                        /* once a second: a line for the log ("" when nothing routed) */
int ain_changed(void);                                     /* the device list changed since the last call (plugged / unplugged) */
void ain_stop_all(void);
int ain_device_present(const char *uid, int want_out);    /* the device is plugged in (and has outputs / inputs) */
unsigned ain_dev_id(const char *uid);                     /* (macOS: its AudioDeviceID; 0 if not there) */
#endif
