/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * prospero-win PS5 audio driver, Unix side (wineps5.so, wineps5.prx on the
 * console).
 *
 * mmdevapi loads it by name like any Wine audio driver: HKCU\Software\Wine\
 * Drivers\Audio = "ps5" (the in-process server's default on the PS5, patch
 * 0120). There is one render endpoint, and one capture endpoint while the
 * title offers a microphone. Each stream keeps mmdevapi's ring of frames in
 * the format its client chose; one Unix thread mixes every started render
 * stream into a grain of the console's format (src/pw_audio_mix.h) and
 * hands it to the title's audio sink (wine/ps5/pw_wine_sink.h), which
 * returns once the console's port has taken it, so the port is the clock.
 * The sink is found with dlsym, as the user driver finds pw_wine_present;
 * without one the driver reports itself unavailable.
 *
 * Capture runs the other way: another Unix thread asks the title's capture
 * source for a grain of the user's DualSense microphone, which returns once
 * the console's port has captured it, and converts it into every started
 * capture stream's ring (src/pw_audio_capture.h). The DualSense's mute
 * button is the system's: a muted microphone's grains arrive as zeros, its
 * packets carry AUDCLNT_BUFFERFLAGS_SILENT, and the endpoint answers the
 * device-mute property (patch 0987) that IAudioEndpointVolume::GetMute
 * reports.
 *
 * The stream, buffer, timing and WoW64 code follows Wine's OSS driver
 * (dlls/wineoss.drv/oss.c, Copyright 2011 Andrew Eikum for CodeWeavers,
 * 2022 Huw Davies), LGPL-2.1-or-later.
 */
#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#include "ntstatus.h"
#include "winternl.h"
#include "initguid.h"
#include "audioclient.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

#include "pw_audio_capture.h"
#include "pw_audio_mix.h"
#include "pw_wine_sink.h"

WINE_DEFAULT_DEBUG_CHANNEL(ps5audio);

struct ps5_stream
{
    EDataFlow flow;
    WAVEFORMATEX *fmt;
    AUDCLNT_SHAREMODE share;
    UINT flags;
    HANDLE event;
    BOOL playing;

    UINT64 written_frames, last_pos_frames;
    UINT32 period_frames, bufsize_frames, tmp_buffer_frames;
    UINT32 period_output_frames, since_event;   /* at the console's rate */
    REFERENCE_TIME period;

    BYTE *local_buffer, *tmp_buffer;
    INT32 getbuf_last; /* <0 when using tmp_buffer */

    /* Render's ring: mix.offset is where valid data starts, mix.held how much. */
    PwAudioMixStream mix;
    /* Capture's ring, the same way round: capture.offset, capture.held. */
    PwAudioCapture capture;
    UINT64 live_end;        /* capture.written after the last grain the microphone was live for */
    UINT64 getbuf_first;    /* the absolute frame the pending capture packet starts at */
    UINT64 dropped_seen;    /* capture.dropped when the last packet was handed out */
    struct ps5_stream *next;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;
static const char device_name[] = "ps5";
static const WCHAR endpoint_name[] = {'P','S','5',0};
/* mmdevapi names it "Microphone (DualSense Wireless Controller)", as
 * Windows names the controller's microphone. */
static const char capture_device_name[] = "ps5mic";
static const WCHAR capture_endpoint_name[] = {'D','u','a','l','S','e','n','s','e',' ','W','i','r','e',
    'l','e','s','s',' ','C','o','n','t','r','o','l','l','e','r',0};

static ULONG_PTR zero_bits = 0;

/* One lock for every stream: the mixer reads them all each grain. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t started = PTHREAD_COND_INITIALIZER;
static pthread_cond_t capture_started = PTHREAD_COND_INITIALIZER;
static struct ps5_stream *streams;
static HANDLE mixer_thread, capture_thread;
static UINT32 capture_rate;     /* the microphone's, once it opened */

static int (*audio_available)(void);
static int (*audio_output)(const int16_t *frames);
static int (*capture_available)(void);
static int (*capture_open)(uint32_t *rate);
static int (*capture_input)(int16_t *frames, uint32_t *state);
static uint32_t (*mic_state)(void);

static NTSTATUS ps5_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static NTSTATUS unlock_result(HRESULT *result, HRESULT value)
{
    *result = value;
    pthread_mutex_unlock(&lock);
    return STATUS_SUCCESS;
}

static struct ps5_stream *handle_get_stream(stream_handle h)
{
    return (struct ps5_stream *)(UINT_PTR)h;
}

/* The title's sink, exported by ntdll.prx on the console. */
static void find_sink(void)
{
    if (audio_output) return;
    audio_available = dlsym(RTLD_DEFAULT, "pw_wine_audio_available");
    audio_output = dlsym(RTLD_DEFAULT, "pw_wine_audio_output");
    if (!audio_available || !audio_output)
    {
        audio_available = NULL;
        audio_output = NULL;
    }
    capture_available = dlsym(RTLD_DEFAULT, "pw_wine_capture_available");
    capture_open = dlsym(RTLD_DEFAULT, "pw_wine_capture_open");
    capture_input = dlsym(RTLD_DEFAULT, "pw_wine_capture_input");
    mic_state = dlsym(RTLD_DEFAULT, "pw_wine_mic_state");
    if (!capture_available || !capture_open || !capture_input || !mic_state)
    {
        capture_available = NULL;
        capture_open = NULL;
        capture_input = NULL;
        mic_state = NULL;
    }
}

static NTSTATUS ps5_process_attach(void *args)
{
    find_sink();
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_test_connect(void *args)
{
    struct test_connect_params *params = args;

    find_sink();
    params->priority = audio_available && audio_available() ? Priority_Preferred : Priority_Unavailable;
    TRACE("priority %d\n", params->priority);
    return STATUS_SUCCESS;
}

static BOOL is_device(const char *device, EDataFlow flow)
{
    if (!device) return FALSE;
    if (flow == eRender) return !strcmp(device, device_name);
    return flow == eCapture && !strcmp(device, capture_device_name);
}

static BOOL microphone_offered(void)
{
    find_sink();
    return capture_available && capture_available();
}

static NTSTATUS ps5_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    const WCHAR *name = endpoint_name;
    const char *device = device_name;
    unsigned int name_size = sizeof(endpoint_name), device_size = sizeof(device_name);
    unsigned int num = params->flow == eRender ? 1 : 0;
    unsigned int offset, needed;

    if (params->flow == eCapture && microphone_offered())
    {
        num = 1;
        name = capture_endpoint_name;
        name_size = sizeof(capture_endpoint_name);
        device = capture_device_name;
        device_size = sizeof(capture_device_name);
    }

    offset = needed = num * sizeof(*params->endpoints);
    if (num)
        needed += name_size + ((device_size + 1) & ~1);

    params->num = num;
    params->default_idx = 0;
    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        return STATUS_SUCCESS;
    }
    if (num)
    {
        params->endpoints[0].name = offset;
        memcpy((char *)params->endpoints + offset, name, name_size);
        offset += name_size;
        params->endpoints[0].device = offset;
        memcpy((char *)params->endpoints + offset, device, device_size);
    }
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* The mixer's view of a Windows format; AUDCLNT_E_UNSUPPORTED_FORMAT for
 * what it cannot play. */
static HRESULT mix_format(const WAVEFORMATEX *fmt, PwAudioMixFormat *out)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
    UINT16 encoding;
    UINT32 mask = 0;

    if (fmt->wFormatTag == WAVE_FORMAT_PCM)
        encoding = PW_AUDIO_MIX_PCM;
    else if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        encoding = PW_AUDIO_MIX_FLOAT;
    else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        if (IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))
            encoding = PW_AUDIO_MIX_PCM;
        else if (IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            encoding = PW_AUDIO_MIX_FLOAT;
        else
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        /* fewer valid bits sit high in the container, which decodes them */
        if (fmtex->Samples.wValidBitsPerSample > fmt->wBitsPerSample)
            return E_INVALIDARG;
        mask = fmtex->dwChannelMask;
    }
    else
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    out->rate = fmt->nSamplesPerSec;
    out->channels = fmt->nChannels;
    out->bits = fmt->wBitsPerSample;
    out->block_align = fmt->nBlockAlign;
    out->encoding = encoding;
    out->channel_mask = mask;
    return pw_audio_mix_format_check(out) == PW_OK ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
}

static WAVEFORMATEXTENSIBLE *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEXTENSIBLE *ret;
    size_t size;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc(size);
    if (!ret)
        return NULL;

    memcpy(ret, fmt, size);

    ret->Format.cbSize = size - sizeof(WAVEFORMATEX);

    return ret;
}

static void free_stream(struct ps5_stream *stream)
{
    SIZE_T size = 0;

    if (stream->local_buffer)
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE);
    size = 0;
    if (stream->tmp_buffer)
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
    free(stream->fmt);
    free(stream);
}

static NTSTATUS ps5_create_stream(void *args)
{
    struct create_stream_params *params = args;
    WAVEFORMATEXTENSIBLE *fmtex;
    struct ps5_stream *stream;
    PwAudioMixFormat format;
    uint32_t rate = 0;
    SIZE_T size;

    if (!is_device(params->device, params->flow))
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (FAILED(params->result = mix_format(params->fmt, &format)))
        return STATUS_SUCCESS;
    /* the microphone opens with its first stream, and stays open */
    if (params->flow == eCapture)
    {
        find_sink();
        if (!capture_open || capture_open(&rate))
        {
            WARN("the title's microphone did not open\n");
            params->result = AUDCLNT_E_DEVICE_INVALIDATED;
            return STATUS_SUCCESS;
        }
        TRACE("microphone at %u Hz\n", rate);
    }

    stream = calloc(1, sizeof(*stream));
    if (!stream)
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    fmtex = clone_format(params->fmt);
    if (!fmtex)
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }
    stream->fmt = &fmtex->Format;
    stream->flow = params->flow;

    stream->period = params->period;
    stream->period_frames = muldiv(params->fmt->nSamplesPerSec, params->period, 10000000);
    stream->period_output_frames = muldiv(PW_WINE_AUDIO_RATE, params->period, 10000000);
    if (stream->period_frames == 0 || stream->period_output_frames == 0)
    {
        params->result = E_INVALIDARG;
        goto exit;
    }

    stream->bufsize_frames = muldiv(params->duration, params->fmt->nSamplesPerSec, 10000000);
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;
    size = stream->bufsize_frames * params->fmt->nBlockAlign;
    if (!stream->bufsize_frames ||
        NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                                &size, MEM_COMMIT, PAGE_READWRITE))
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }
    if (params->flow == eCapture
        ? pw_audio_capture_init(&stream->capture, &format, stream->local_buffer, stream->bufsize_frames, rate)
        : pw_audio_mix_stream_init(&stream->mix, &format, stream->local_buffer, stream->bufsize_frames))
    {
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        goto exit;
    }

    stream->share = params->share;
    stream->flags = params->flags;

    pthread_mutex_lock(&lock);
    if (params->flow == eCapture) capture_rate = rate;
    stream->next = streams;
    streams = stream;
    pthread_mutex_unlock(&lock);

exit:
    if (FAILED(params->result))
        free_stream(stream);
    else
    {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS ps5_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream), **link;

    /* the mixer only touches streams under the lock */
    pthread_mutex_lock(&lock);
    for (link = &streams; *link; link = &(*link)->next)
        if (*link == stream)
        {
            *link = stream->next;
            break;
        }
    pthread_mutex_unlock(&lock);

    free_stream(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* The mixer: every started stream into one grain, then the grain to the
 * title, which returns once the console's port took it. */
static void mixer_loop(void *args)
{
    static float accumulator[2 * PW_WINE_AUDIO_GRAIN];
    static int16_t grain[2 * PW_WINE_AUDIO_GRAIN];
    struct ps5_stream *stream;
    BOOL playing;

    pthread_mutex_lock(&lock);
    for (;;)
    {
        playing = FALSE;
        for (stream = streams; stream; stream = stream->next)
            playing |= stream->playing && stream->flow == eRender;
        if (!playing)
        {
            pthread_cond_wait(&started, &lock);
            continue;
        }

        memset(accumulator, 0, sizeof(accumulator));
        for (stream = streams; stream; stream = stream->next)
        {
            if (!stream->playing || stream->flow != eRender) continue;
            pw_audio_mix_add(&stream->mix, accumulator, PW_WINE_AUDIO_GRAIN);
            stream->since_event += PW_WINE_AUDIO_GRAIN;
            if (stream->since_event >= stream->period_output_frames)
            {
                stream->since_event %= stream->period_output_frames;
                if (stream->event) NtSetEvent(stream->event, NULL);
            }
        }
        pw_audio_mix_store(accumulator, grain, PW_WINE_AUDIO_GRAIN);
        pthread_mutex_unlock(&lock);

        if (!audio_output || audio_output(grain))
        {
            /* the port refused it: keep time without it */
            LARGE_INTEGER delay;
            delay.QuadPart = -(LONGLONG)PW_WINE_AUDIO_GRAIN * 10000000 / PW_WINE_AUDIO_RATE;
            NtDelayExecution(FALSE, &delay);
        }
        pthread_mutex_lock(&lock);
    }
}

/* The microphone: one grain of the title's capture source into every
 * started capture stream. The source returns once the console's port has
 * captured the grain, so the port is the clock. */
static void capture_loop(void *args)
{
    static int16_t grain[PW_WINE_CAPTURE_GRAIN];
    struct ps5_stream *stream;
    uint32_t state;
    UINT32 added, rate;
    BOOL recording, live;

    pthread_mutex_lock(&lock);
    for (;;)
    {
        recording = FALSE;
        for (stream = streams; stream; stream = stream->next)
            recording |= stream->playing && stream->flow == eCapture;
        if (!recording)
        {
            pthread_cond_wait(&capture_started, &lock);
            continue;
        }
        rate = capture_rate ? capture_rate : 16000;
        pthread_mutex_unlock(&lock);

        /* a failed grain is zeros (the sink's), and a muted one too */
        live = !capture_input(grain, &state);
        if (!live)
        {
            /* the port refused: keep the microphone's time without it */
            LARGE_INTEGER delay;
            delay.QuadPart = -(LONGLONG)PW_WINE_CAPTURE_GRAIN * 10000000 / rate;
            NtDelayExecution(FALSE, &delay);
        }
        else if (state == PW_WINE_MIC_MUTED || state == PW_WINE_MIC_NO_DEVICE)
            live = FALSE;

        pthread_mutex_lock(&lock);
        for (stream = streams; stream; stream = stream->next)
        {
            if (!stream->playing || stream->flow != eCapture) continue;
            added = pw_audio_capture_put(&stream->capture, grain, PW_WINE_CAPTURE_GRAIN);
            if (live) stream->live_end = stream->capture.written;
            stream->since_event += added;
            if (stream->since_event >= stream->period_frames)
            {
                stream->since_event %= stream->period_frames;
                if (stream->event) NtSetEvent(stream->event, NULL);
            }
        }
    }
}

static NTSTATUS ps5_start(void *args)
{
    struct start_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);
    static const WCHAR name[] = {'p','s','5','_','a','u','d','i','o','_','m','i','x','e','r',0};
    static const WCHAR capture_name[] = {'p','s','5','_','a','u','d','i','o','_','c','a','p','t','u','r','e',0};

    pthread_mutex_lock(&lock);

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return unlock_result(&params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET);

    if (stream->playing)
        return unlock_result(&params->result, AUDCLNT_E_NOT_STOPPED);

    if (stream->flow == eCapture)
    {
        if (!capture_thread && create_unix_thread(&capture_thread, capture_name, capture_loop, NULL))
        {
            capture_thread = NULL;
            return unlock_result(&params->result, E_OUTOFMEMORY);
        }
    }
    else if (!mixer_thread && create_unix_thread(&mixer_thread, name, mixer_loop, NULL))
    {
        mixer_thread = NULL;
        return unlock_result(&params->result, E_OUTOFMEMORY);
    }
    stream->playing = TRUE;
    stream->since_event = 0;
    pthread_cond_signal(stream->flow == eCapture ? &capture_started : &started);

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_stop(void *args)
{
    struct stop_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    if (!stream->playing)
        return unlock_result(&params->result, S_FALSE);

    stream->playing = FALSE;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_reset(void *args)
{
    struct reset_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    if (stream->playing)
        return unlock_result(&params->result, AUDCLNT_E_NOT_STOPPED);

    if (stream->getbuf_last)
        return unlock_result(&params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING);

    stream->since_event = 0;
    if (stream->flow == eCapture)
    {
        /* what was captured is forgotten; the position keeps counting */
        pw_audio_capture_take(&stream->capture, stream->capture.held);
        return unlock_result(&params->result, S_OK);
    }
    stream->written_frames = 0;
    stream->last_pos_frames = 0;
    pw_audio_mix_stream_reset(&stream->mix);

    return unlock_result(&params->result, S_OK);
}

static void silence_buffer(struct ps5_stream *stream, BYTE *buffer, UINT32 frames)
{
    memset(buffer, stream->mix.format.encoding == PW_AUDIO_MIX_PCM && stream->mix.format.bits == 8 ? 128 : 0,
           frames * stream->fmt->nBlockAlign);
}

static NTSTATUS ps5_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);
    UINT32 write_pos, frames = params->frames;
    BYTE **data = params->data;
    SIZE_T size;

    pthread_mutex_lock(&lock);

    if (stream->getbuf_last)
        return unlock_result(&params->result, AUDCLNT_E_OUT_OF_ORDER);

    if (!frames)
        return unlock_result(&params->result, S_OK);

    if (stream->mix.held + frames > stream->bufsize_frames)
        return unlock_result(&params->result, AUDCLNT_E_BUFFER_TOO_LARGE);

    write_pos = (stream->mix.offset + stream->mix.held) % stream->bufsize_frames;
    if (write_pos + frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                        &size, MEM_COMMIT, PAGE_READWRITE))
            {
                stream->tmp_buffer_frames = 0;
                return unlock_result(&params->result, E_OUTOFMEMORY);
            }
            stream->tmp_buffer_frames = frames;
        }
        *data = stream->tmp_buffer;
        stream->getbuf_last = -frames;
    }
    else
    {
        *data = stream->local_buffer + write_pos * stream->fmt->nBlockAlign;
        stream->getbuf_last = frames;
    }

    silence_buffer(stream, *data, frames);

    return unlock_result(&params->result, S_OK);
}

static void wrap_buffer(struct ps5_stream *stream, BYTE *buffer, UINT32 written_frames)
{
    UINT32 write_offs_frames = (stream->mix.offset + stream->mix.held) % stream->bufsize_frames;
    UINT32 write_offs_bytes = write_offs_frames * stream->fmt->nBlockAlign;
    UINT32 chunk_frames = stream->bufsize_frames - write_offs_frames;
    UINT32 chunk_bytes = chunk_frames * stream->fmt->nBlockAlign;
    UINT32 written_bytes = written_frames * stream->fmt->nBlockAlign;

    if (written_bytes <= chunk_bytes)
        memcpy(stream->local_buffer + write_offs_bytes, buffer, written_bytes);
    else
    {
        memcpy(stream->local_buffer + write_offs_bytes, buffer, chunk_bytes);
        memcpy(stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes);
    }
}

static NTSTATUS ps5_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);
    UINT32 written_frames = params->written_frames;
    BYTE *buffer;

    pthread_mutex_lock(&lock);

    if (!written_frames)
    {
        stream->getbuf_last = 0;
        return unlock_result(&params->result, S_OK);
    }

    if (!stream->getbuf_last)
        return unlock_result(&params->result, AUDCLNT_E_OUT_OF_ORDER);

    if (written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        return unlock_result(&params->result, AUDCLNT_E_INVALID_SIZE);

    if (stream->getbuf_last >= 0)
        buffer = stream->local_buffer + stream->fmt->nBlockAlign *
                 ((stream->mix.offset + stream->mix.held) % stream->bufsize_frames);
    else
        buffer = stream->tmp_buffer;

    if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer(stream, buffer, written_frames);

    if (stream->getbuf_last < 0)
        wrap_buffer(stream, buffer, written_frames);

    stream->mix.held += written_frames;
    stream->written_frames += written_frames;
    stream->getbuf_last = 0;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);
    UINT64 *devpos = params->devpos, *qpcpos = params->qpcpos;
    UINT32 *frames = params->frames;
    UINT *flags = params->flags;
    BYTE **data = params->data;
    UINT32 chunk;
    UINT64 first;
    SIZE_T size;

    pthread_mutex_lock(&lock);

    if (stream->getbuf_last)
        return unlock_result(&params->result, AUDCLNT_E_OUT_OF_ORDER);

    if (stream->capture.held < stream->period_frames)
    {
        *frames = 0;
        return unlock_result(&params->result, AUDCLNT_S_BUFFER_EMPTY);
    }

    *frames = stream->period_frames;
    first = stream->capture.written - stream->capture.held;
    *flags = 0;
    /* every frame of it came while the microphone was muted (or gone) */
    if (first >= stream->live_end)
        *flags |= AUDCLNT_BUFFERFLAGS_SILENT;
    /* frames were lost to a full ring since the last packet */
    if (stream->capture.dropped != stream->dropped_seen)
    {
        *flags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
        stream->dropped_seen = stream->capture.dropped;
    }

    if (stream->capture.offset + *frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < *frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = *frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                        &size, MEM_COMMIT, PAGE_READWRITE))
            {
                stream->tmp_buffer_frames = 0;
                *frames = 0;
                return unlock_result(&params->result, E_OUTOFMEMORY);
            }
            stream->tmp_buffer_frames = *frames;
        }
        chunk = stream->bufsize_frames - stream->capture.offset;
        memcpy(stream->tmp_buffer, stream->local_buffer + stream->capture.offset * stream->fmt->nBlockAlign,
               chunk * stream->fmt->nBlockAlign);
        memcpy(stream->tmp_buffer + chunk * stream->fmt->nBlockAlign, stream->local_buffer,
               (*frames - chunk) * stream->fmt->nBlockAlign);
        *data = stream->tmp_buffer;
    }
    else
        *data = stream->local_buffer + stream->capture.offset * stream->fmt->nBlockAlign;

    stream->getbuf_last = *frames;
    stream->getbuf_first = first;

    if (devpos)
        *devpos = first;
    if (qpcpos)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter(&stamp, &freq);
        *qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);
    UINT32 done = params->done;
    UINT64 gone;

    pthread_mutex_lock(&lock);

    if (!done)
    {
        stream->getbuf_last = 0;
        return unlock_result(&params->result, S_OK);
    }

    if (!stream->getbuf_last)
        return unlock_result(&params->result, AUDCLNT_E_OUT_OF_ORDER);

    if (stream->getbuf_last != done)
        return unlock_result(&params->result, AUDCLNT_E_INVALID_SIZE);

    /* a ring that overran meanwhile already dropped the packet's oldest frames */
    gone = stream->capture.written - stream->capture.held - stream->getbuf_first;
    if (gone < done)
        pw_audio_capture_take(&stream->capture, done - (UINT32)gone);
    stream->written_frames += done;
    stream->getbuf_last = 0;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;
    PwAudioMixFormat format;

    if (!is_device(params->device, params->flow))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else
        params->result = mix_format(params->fmt_in, &format);
    return STATUS_SUCCESS;
}

/* What the mixer adds in: float stereo at the console's rate; the
 * microphone's: float mono at the same rate. */
static NTSTATUS ps5_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if (!is_device(params->device, params->flow))
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    /* the microphone is mono; it is resampled to whatever rate a client asks for */
    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.nChannels = params->flow == eCapture ? 1 : 2;
    fmt->Format.nSamplesPerSec = PW_WINE_AUDIO_RATE;
    fmt->Format.wBitsPerSample = 32;
    fmt->Format.nBlockAlign = fmt->Format.nChannels * fmt->Format.wBitsPerSample / 8;
    fmt->Format.nAvgBytesPerSec = fmt->Format.nSamplesPerSec * fmt->Format.nBlockAlign;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt->Samples.wValidBitsPerSample = fmt->Format.wBitsPerSample;
    fmt->dwChannelMask = params->flow == eCapture ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    if (params->def_period)
        *params->def_period = def_period;
    if (params->min_period)
        *params->min_period = min_period;

    params->result = S_OK;

    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    *params->frames = stream->bufsize_frames;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    /* a period, plus the grain the port holds while another plays (or
     * captures) */
    if (stream->flow == eCapture)
        *params->latency = stream->period +
                           (REFERENCE_TIME)PW_WINE_CAPTURE_GRAIN * 10000000 / stream->capture.source_rate;
    else
        *params->latency = stream->period + (REFERENCE_TIME)PW_WINE_AUDIO_GRAIN * 10000000 / PW_WINE_AUDIO_RATE;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    *params->padding = stream->flow == eCapture ? stream->capture.held : stream->mix.held;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    *params->frames = (stream->flow == eCapture ? stream->capture.held : stream->mix.held) < stream->period_frames
                      ? 0 : stream->period_frames;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_get_position(void *args)
{
    struct get_position_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);
    UINT64 *pos = params->pos, *qpctime = params->qpctime;

    if (params->device)
    {
        FIXME("Device position reporting not implemented\n");
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    pthread_mutex_lock(&lock);

    /* capture: every frame the microphone gave the stream */
    if (stream->flow == eCapture)
        *pos = stream->capture.written;
    else
        *pos = stream->written_frames - stream->mix.held;
    if (*pos < stream->last_pos_frames)
        *pos = stream->last_pos_frames;
    stream->last_pos_frames = *pos;

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *pos *= stream->fmt->nBlockAlign;

    if (qpctime)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter(&stamp, &freq);
        *qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);
    float gain[PW_AUDIO_MIX_MAX_CHANNELS];
    UINT16 i;

    pthread_mutex_lock(&lock);
    for (i = 0; i < stream->fmt->nChannels; ++i)
        gain[i] = params->master_volume * params->volumes[i] * params->session_volumes[i];
    if (stream->flow == eCapture)
        pw_audio_capture_set_gain(&stream->capture, gain, stream->fmt->nChannels);
    else
        pw_audio_mix_set_gain(&stream->mix, gain, stream->fmt->nChannels);
    pthread_mutex_unlock(&lock);

    return STATUS_SUCCESS;
}

static NTSTATUS ps5_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        return unlock_result(&params->result, AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED);

    if (stream->event)
    {
        FIXME("called twice\n");
        return unlock_result(&params->result, HRESULT_FROM_WIN32(ERROR_INVALID_NAME));
    }

    stream->event = params->event;

    return unlock_result(&params->result, S_OK);
}

/* IAudioClockAdjustment: the same frames played at another rate. */
static NTSTATUS ps5_set_sample_rate(void *args)
{
    struct set_sample_rate_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    if (!(stream->flags & AUDCLNT_STREAMFLAGS_RATEADJUST) || stream->flow == eCapture)
        return unlock_result(&params->result, AUDCLNT_E_UNSUPPORTED_FORMAT);
    if (params->rate < PW_AUDIO_MIX_MIN_RATE || params->rate > PW_AUDIO_MIX_MAX_RATE)
        return unlock_result(&params->result, E_INVALIDARG);
    pw_audio_mix_set_rate(&stream->mix, (UINT32)params->rate);

    return unlock_result(&params->result, S_OK);
}

static NTSTATUS ps5_is_started(void *args)
{
    struct is_started_params *params = args;
    struct ps5_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&lock);

    return unlock_result(&params->result, stream->playing ? S_OK : S_FALSE);
}

/* The microphone's mute switch: 1 when the system keeps it muted (the
 * DualSense's mute button), as the property mmdevapi's endpoint volume
 * reports beside its own mute (patch 0987). Nothing else is the driver's. */
static BOOL is_mute_key(const PROPERTYKEY *prop)
{
    static const PROPERTYKEY device_mute = WINE_PKEY_AUDIOENDPOINT_DEVICE_MUTE;

    return prop && prop->pid == device_mute.pid && !memcmp(&prop->fmtid, &device_mute.fmtid, sizeof(GUID));
}

static BOOL microphone_muted(void)
{
    find_sink();
    return mic_state && mic_state() == PW_WINE_MIC_MUTED;
}

static NTSTATUS ps5_get_prop_value(void *args)
{
    struct get_prop_value_params *params = args;

    if (is_device(params->device, eCapture) && params->flow == eCapture && is_mute_key(params->prop))
    {
        params->value->vt = VT_BOOL;
        params->value->boolVal = microphone_muted() ? VARIANT_TRUE : VARIANT_FALSE;
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    params->result = E_NOTIMPL;

    return STATUS_SUCCESS;
}

/* No MIDI and no auxiliary devices: every count is 0, every device is bad. */
static NTSTATUS ps5_midi_init(void *args)
{
    struct midi_init_params *params = args;

    *params->err = DRV_SUCCESS;
    return STATUS_SUCCESS;
}

static UINT no_device(UINT msg, UINT count_msg)
{
    return msg == count_msg ? 0 : MMSYSERR_BADDEVICEID;
}

static NTSTATUS ps5_midi_out_message(void *args)
{
    struct midi_out_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = no_device(params->msg, MODM_GETNUMDEVS);
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_midi_in_message(void *args)
{
    struct midi_in_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = no_device(params->msg, MIDM_GETNUMDEVS);
    return STATUS_SUCCESS;
}

/* Nothing ever notifies: the notify thread may quit at once. */
static NTSTATUS ps5_midi_notify_wait(void *args)
{
    struct midi_notify_wait_params *params = args;

    *params->quit = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_aux_message(void *args)
{
    struct aux_message_params *params = args;

    *params->err = no_device(params->msg, AUXDM_GETNUMDEVS);
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    ps5_process_attach,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_get_endpoint_ids,
    ps5_create_stream,
    ps5_release_stream,
    ps5_start,
    ps5_stop,
    ps5_reset,
    ps5_get_render_buffer,
    ps5_release_render_buffer,
    ps5_get_capture_buffer,
    ps5_release_capture_buffer,
    ps5_is_format_supported,
    ps5_not_implemented,
    ps5_get_mix_format,
    ps5_get_device_period,
    ps5_get_buffer_size,
    ps5_get_latency,
    ps5_get_current_padding,
    ps5_get_next_packet_size,
    ps5_get_frequency,
    ps5_get_position,
    ps5_set_volumes,
    ps5_set_event_handle,
    ps5_set_sample_rate,
    ps5_test_connect,
    ps5_is_started,
    ps5_get_prop_value,
    ps5_not_implemented,
    ps5_midi_init,
    ps5_not_implemented,
    ps5_midi_out_message,
    ps5_midi_in_message,
    ps5_midi_notify_wait,
    ps5_aux_message,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS ps5_wow64_process_attach(void *args)
{
    SYSTEM_BASIC_INFORMATION info;

    NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    find_sink();
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_test_connect(void *args)
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name),
    };
    ps5_test_connect(&params);
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    ps5_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        UINT flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    ps5_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
    };
    ps5_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    ps5_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .data = &data,
        .frames = ULongToPtr(params32->frames),
        .flags = ULongToPtr(params32->flags),
        .devpos = ULongToPtr(params32->devpos),
        .qpcpos = ULongToPtr(params32->qpcpos)
    };
    ps5_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    ps5_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    ps5_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    ps5_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    ps5_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    ps5_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    ps5_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    ps5_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    ps5_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    ps5_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return ps5_set_volumes(&params);
}

static NTSTATUS ps5_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };

    ps5_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_prop_value(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer;
        PTR32 buffer_size;
    } *params32 = args;
    /* a PROPVARIANT's type and value sit where they do in 64 bits; only
     * VT_BOOL is answered */
    struct
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        VARIANT_BOOL boolVal;
    } *value32;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
    };

    ps5_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = ULongToPtr(params32->value);
        value32->vt = value.vt;
        value32->boolVal = value.boolVal;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_midi_init(void *args)
{
    struct
    {
        PTR32 err;
    } *params32 = args;

    *(UINT *)ULongToPtr(params32->err) = DRV_SUCCESS;
    return STATUS_SUCCESS;
}

/* The in and out messages share this layout; a notify context starts with
 * send_notify in both widths. */
struct midi_message_params32
{
    UINT dev_id;
    UINT msg;
    UINT user;
    UINT param_1;
    UINT param_2;
    PTR32 err;
    PTR32 notify;
};

static NTSTATUS ps5_wow64_midi_out_message(void *args)
{
    struct midi_message_params32 *params32 = args;

    *(BOOL *)ULongToPtr(params32->notify) = FALSE;
    *(UINT *)ULongToPtr(params32->err) = no_device(params32->msg, MODM_GETNUMDEVS);
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_midi_in_message(void *args)
{
    struct midi_message_params32 *params32 = args;

    *(BOOL *)ULongToPtr(params32->notify) = FALSE;
    *(UINT *)ULongToPtr(params32->err) = no_device(params32->msg, MIDM_GETNUMDEVS);
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_midi_notify_wait(void *args)
{
    struct
    {
        PTR32 quit;
        PTR32 notify;
    } *params32 = args;

    *(BOOL *)ULongToPtr(params32->quit) = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_aux_message(void *args)
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
    } *params32 = args;

    *(UINT *)ULongToPtr(params32->err) = no_device(params32->msg, AUXDM_GETNUMDEVS);
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    ps5_wow64_process_attach,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_wow64_get_endpoint_ids,
    ps5_wow64_create_stream,
    ps5_wow64_release_stream,
    ps5_start,
    ps5_stop,
    ps5_reset,
    ps5_wow64_get_render_buffer,
    ps5_release_render_buffer,
    ps5_wow64_get_capture_buffer,
    ps5_release_capture_buffer,
    ps5_wow64_is_format_supported,
    ps5_not_implemented,
    ps5_wow64_get_mix_format,
    ps5_wow64_get_device_period,
    ps5_wow64_get_buffer_size,
    ps5_wow64_get_latency,
    ps5_wow64_get_current_padding,
    ps5_wow64_get_next_packet_size,
    ps5_wow64_get_frequency,
    ps5_wow64_get_position,
    ps5_wow64_set_volumes,
    ps5_wow64_set_event_handle,
    ps5_set_sample_rate,
    ps5_wow64_test_connect,
    ps5_is_started,
    ps5_wow64_get_prop_value,
    ps5_not_implemented,
    ps5_wow64_midi_init,
    ps5_not_implemented,
    ps5_wow64_midi_out_message,
    ps5_wow64_midi_in_message,
    ps5_wow64_midi_notify_wait,
    ps5_wow64_aux_message,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
