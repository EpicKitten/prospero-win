/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_CAPTURE_H
#define PW_AUDIO_CAPTURE_H
#include "pw_audio_mix.h"

/* The capture side of Wine's PS5 audio driver (wine/wineps5): it turns the
 * console's microphone, mono 16-bit grains at the rate its port opened
 * with, into the PCM format each Windows recording stream chose.
 *
 * Each stream owns a ring of frames in its own format (mmdevapi's local
 * buffer). Putting a grain resamples it linearly to the stream's rate,
 * applies the stream's per-channel gain, encodes the sample once per
 * channel and appends the frames to the ring. A full ring drops its oldest
 * frames, as a Windows capture endpoint overruns: the newest sound wins.
 *
 * Portable: no allocation and no locking. The driver owns the buffers,
 * serialises access to a stream and runs the capture thread. */

typedef struct PwAudioCapture {
    PwAudioMixFormat format;    /* the stream's; checked as the mixer checks it */
    uint8_t *frames;            /* ring of capacity frames, block_align bytes each */
    uint32_t capacity;
    uint32_t offset,held;       /* the oldest unread frame, and how many there are */
    uint64_t written;           /* frames appended since init or reset */
    uint64_t dropped;           /* frames lost to a full ring since init or reset */
    uint32_t source_rate;       /* the microphone's frames per second */
    float gain[PW_AUDIO_MIX_MAX_CHANNELS];
    uint64_t step,phase;        /* source frames per stream frame, and position: 32.32 */
    float previous,current;
} PwAudioCapture;

/* A stream of format writing the ring frames[capacity], empty, at unity
 * gain, fed from a microphone of source_rate frames per second.
 * PW_ERR_UNSUPPORTED for a format or rate the converter cannot write. */
int pw_audio_capture_init(PwAudioCapture *capture,const PwAudioMixFormat *format,
                          uint8_t *frames,uint32_t capacity,uint32_t source_rate);
/* Forget the held frames and the resampler's history (a stopped stream's Reset). */
void pw_audio_capture_reset(PwAudioCapture *capture);
/* One gain per channel of the stream's format, 0 (silent) to 1 (unity). */
int pw_audio_capture_set_gain(PwAudioCapture *capture,const float *gain,uint32_t count);
/* Append count mono samples of the microphone, resampled to the stream's
 * rate; samples may be NULL for that much silence. Returns how many frames
 * the ring gained; a full ring drops its oldest frames to make room. */
uint32_t pw_audio_capture_put(PwAudioCapture *capture,const int16_t *samples,uint32_t count);
/* The frames the reader took from the front of the ring (at most held). */
void pw_audio_capture_take(PwAudioCapture *capture,uint32_t frames);

#endif
