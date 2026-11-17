/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_IN_PS5_H
#define PW_AUDIO_IN_PS5_H
/*
 * The console's microphone for one user: the DualSense's own, or a headset
 * plugged into it, whichever the system routes to that user's voice input.
 * Wine's audio driver (wine/wineps5) records from it through the title's
 * capture source, one grain of 16-bit mono at a time; each read returns
 * once the port has captured a grain, so the port clocks the driver.
 *
 * The DualSense's mute button belongs to the system: it silences the
 * user's input itself and the title only sees it in the port's silent
 * state. Every read asks for that state, and a grain read while the port
 * is silent is delivered as zeros whatever the port returned.
 */
#include "../include/prospero_win.h"
#include <stdint.h>

enum { PW_AUDIO_IN_PS5_GRAIN=256 };
/* What the port's silent state says about the microphone. */
enum {
    PW_AUDIO_IN_PS5_UNKNOWN=0,      /* not open, or the state could not be read */
    PW_AUDIO_IN_PS5_LIVE=1,         /* capturing */
    PW_AUDIO_IN_PS5_MUTED=2,        /* the system silences it: the mute button, or another
                                       application (party chat) has priority */
    PW_AUDIO_IN_PS5_NO_DEVICE=3,    /* the user has no microphone (no controller connected) */
};
/* sceAudioInGetSilentState's bit for a missing device; any other bit set
 * means the system keeps the port silent. */
enum { PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE=0x1 };

typedef struct PwAudioInPs5Ops {
    int (*open)(int32_t user_id,uint32_t type,uint32_t index,uint32_t grain,
                uint32_t rate,uint32_t param);
    /* Blocks until one grain is captured; negative on failure. */
    int (*input)(int32_t handle,void *samples);
    /* Optional: sceAudioInGetSilentState, NULL if none (the state then
     * reads as unknown and grains are delivered as captured). */
    int (*silent_state)(int32_t handle);
    int (*close)(int32_t handle);
} PwAudioInPs5Ops;

typedef struct PwAudioInPs5 {
    PwAudioInPs5Ops ops;
    int32_t user_id,handle;         /* -1 while closed */
    uint32_t type,rate;             /* the port that opened */
    int open_rc,last_input_rc,last_silent;
    uint32_t state;                 /* PW_AUDIO_IN_PS5_*, the newest; atomic */
    uint64_t grains,silent_grains,failed_grains;
} PwAudioInPs5;

int pw_audio_in_ps5_platform_ops(PwAudioInPs5Ops *);
/* The user's voice input: a voice-chat port at 16 kHz, or else a general
 * one at 48 kHz, PW_AUDIO_IN_PS5_GRAIN frames of 16-bit mono per read.
 * PW_ERR_PRECONDITION without the required ops or a user, PW_ERR_STATE
 * when no port opens (open_rc keeps the last refusal). */
int pw_audio_in_ps5_open(PwAudioInPs5 *,const PwAudioInPs5Ops *,int32_t user_id);
/* PW_AUDIO_IN_PS5_* for a silent state (negative: the call failed). */
uint32_t pw_audio_in_ps5_classify(int silent_state);
/* The state now, without capturing; safe beside a read on another thread. */
uint32_t pw_audio_in_ps5_poll(PwAudioInPs5 *);
/* One grain, and the state it was captured in: zeros unless LIVE or
 * UNKNOWN. PW_ERR_STATE (and zeros) when the port fails, PW_ERR_PRECONDITION
 * when it is not open. */
int pw_audio_in_ps5_read(PwAudioInPs5 *,int16_t grain[PW_AUDIO_IN_PS5_GRAIN],uint32_t *state);
int pw_audio_in_ps5_close(PwAudioInPs5 *);

#endif
