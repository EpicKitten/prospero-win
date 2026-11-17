/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine's PS5 audio driver (wine/wineps5/unix.c) recording from the title's
 * microphone, on the host: the driver's own code, the real sink
 * (wine/ps5/pw_wine_sink.c, which the driver finds with dlsym as it finds
 * ntdll's on the console) and a fake microphone that captures a ramp. The
 * few ntdll calls the driver makes are stood in for below.
 *
 * Built and run by tests/test_wineps5_capture.py against the pinned Wine's
 * headers with patch 0987 applied.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "../wine/wineps5/unix.c"

/* ---- what ntdll provides on the console ---------------------------------- */

NTSTATUS WINAPI NtAllocateVirtualMemory(HANDLE process, PVOID *addr, ULONG_PTR zero_bits, SIZE_T *size,
                                        ULONG type, ULONG protect)
{
    *addr = calloc(1, *size);
    return *addr ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
NTSTATUS WINAPI NtFreeVirtualMemory(HANDLE process, PVOID *addr, SIZE_T *size, ULONG type)
{
    free(*addr);
    *addr = NULL;
    return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtDelayExecution(BOOLEAN alertable, const LARGE_INTEGER *timeout)
{
    usleep((useconds_t)(-timeout->QuadPart / 10));
    return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtQueryPerformanceCounter(PLARGE_INTEGER counter, PLARGE_INTEGER frequency)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    counter->QuadPart = (LONGLONG)now.tv_sec * 10000000 + now.tv_nsec / 100;
    if (frequency) frequency->QuadPart = 10000000;
    return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtQuerySystemInformation(SYSTEM_INFORMATION_CLASS class, PVOID info, ULONG size, PULONG ret)
{
    return STATUS_NOT_IMPLEMENTED;
}
/* An event is a counter here. */
static LONG events;
NTSTATUS WINAPI NtSetEvent(HANDLE handle, LONG *previous)
{
    __atomic_add_fetch((LONG *)handle, 1, __ATOMIC_RELAXED);
    return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtSetInformationThread(HANDLE handle, THREADINFOCLASS class, LPCVOID info, ULONG size)
{
    return STATUS_SUCCESS;
}
void WINAPI RtlInitUnicodeString(PUNICODE_STRING string, PCWSTR source)
{
}
struct thread_start { void (*entry)(void *); void *param; };
static void *thread_main(void *arg)
{
    struct thread_start start = *(struct thread_start *)arg;
    free(arg);
    start.entry(start.param);
    return NULL;
}
NTSTATUS WINAPI PsCreateSystemThread(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES attr, HANDLE process,
                                     PCLIENT_ID id, PRTL_THREAD_START_ROUTINE entry, PVOID param)
{
    struct thread_start *start = malloc(sizeof(*start));
    pthread_t thread;
    start->entry = (void (*)(void *))entry;
    start->param = param;
    if (pthread_create(&thread, NULL, thread_main, start)) return STATUS_NO_MEMORY;
    pthread_detach(thread);
    *handle = (HANDLE)1;
    return STATUS_SUCCESS;
}
int __cdecl __wine_dbg_output(const char *str) { return 0; }
int __cdecl __wine_dbg_header(enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                              const char *function) { return -1; }

/* ---- the title's microphone ------------------------------------------------ */

static uint32_t mic_now = PW_WINE_MIC_LIVE;    /* atomic: the test sets it */
static volatile int mic_refuse;
static int next_sample;
static int mic_open(void *context, uint32_t *rate)
{
    if (mic_refuse) return -4;
    *rate = 16000;
    return 0;
}
/* A grain of a rising ramp (1..30000, round) at the port's pace: 16 ms. */
static int mic_input(void *context, int16_t *frames, uint32_t *state)
{
    usleep(16000);
    for (int i = 0; i < PW_WINE_CAPTURE_GRAIN; i++) frames[i] = (int16_t)(next_sample++ % 30000 + 1);
    *state = __atomic_load_n(&mic_now, __ATOMIC_RELAXED);
    return 0;
}
static uint32_t mic_poll(void *context) { return __atomic_load_n(&mic_now, __ATOMIC_RELAXED); }

/* ---- the driver, as mmdevapi calls it ------------------------------------- */

static void call(enum unix_funcs code, void *params)
{
    assert(!__wine_unix_call_funcs[code](params));
}

static unsigned endpoints(EDataFlow flow, WCHAR *name, char *device)
{
    union { struct endpoint e; char bytes[256]; } buffer;
    struct get_endpoint_ids_params params = { .flow = flow, .endpoints = &buffer.e, .size = sizeof(buffer) };
    call(get_endpoint_ids, &params);
    assert(params.result == S_OK && params.default_idx == 0);
    if (params.num && name) memcpy(name, buffer.bytes + buffer.e.name, 64);
    if (params.num && device) strcpy(device, buffer.bytes + buffer.e.device);
    return params.num;
}

static BOOL muted_property(void)
{
    static const PROPERTYKEY key = WINE_PKEY_AUDIOENDPOINT_DEVICE_MUTE;
    PROPVARIANT value;
    struct get_prop_value_params params = { .device = "ps5mic", .flow = eCapture, .prop = &key, .value = &value };
    call(get_prop_value, &params);
    assert(params.result == S_OK && value.vt == VT_BOOL);
    return value.boolVal != VARIANT_FALSE;
}

static stream_handle create(const WAVEFORMATEX *fmt, HRESULT *result)
{
    stream_handle stream = 0;
    UINT32 channels = 0;
    struct create_stream_params params = {
        .device = "ps5mic", .flow = eCapture, .share = AUDCLNT_SHAREMODE_SHARED,
        .flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK, .duration = 1000000, .period = 100000, .fmt = fmt,
        .channel_count = &channels, .stream = &stream,
    };
    call(create_stream, &params);
    *result = params.result;
    if (SUCCEEDED(params.result)) assert(channels == fmt->nChannels);
    return stream;
}

/* The next packet: S_OK, or AUDCLNT_S_BUFFER_EMPTY after a second. */
static HRESULT packet(stream_handle stream, BYTE **data, UINT32 *frames, UINT *flags, UINT64 *devpos)
{
    for (int i = 0; i < 1000; i++)
    {
        UINT64 qpc;
        struct get_capture_buffer_params params = { .stream = stream, .data = data, .frames = frames,
                                                    .flags = flags, .devpos = devpos, .qpcpos = &qpc };
        call(get_capture_buffer, &params);
        if (params.result != AUDCLNT_S_BUFFER_EMPTY) return params.result;
        assert(*frames == 0);
        usleep(1000);
    }
    return AUDCLNT_S_BUFFER_EMPTY;
}

static HRESULT release(stream_handle stream, UINT32 done)
{
    struct release_capture_buffer_params params = { .stream = stream, .done = done };
    call(release_capture_buffer, &params);
    return params.result;
}

int main(void)
{
    WCHAR name[32];
    char device[16];
    PwWineCaptureSource source = { mic_open, mic_input, mic_poll };
    WAVEFORMATEX fmt = { WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0 };
    WAVEFORMATEXTENSIBLE mix;
    stream_handle stream;
    HRESULT result;
    BYTE *data;
    UINT32 frames, padding;
    UINT flags;
    UINT64 devpos, last_devpos;

    call(process_attach, NULL);

    /* No microphone from the title: render only. */
    assert(endpoints(eRender, NULL, device) == 1 && !strcmp(device, "ps5"));
    assert(endpoints(eCapture, NULL, NULL) == 0);

    /* The title's microphone is the controller's capture endpoint. */
    pw_wine_set_capture_source(&source, NULL);
    assert(endpoints(eCapture, name, device) == 1 && !strcmp(device, "ps5mic"));
    assert(!memcmp(name, capture_endpoint_name, sizeof(capture_endpoint_name)));
    assert(endpoints(eRender, NULL, device) == 1 && !strcmp(device, "ps5"));

    /* Its mix format: float mono at 48 kHz; PCM formats convert. */
    {
        struct get_mix_format_params params = { .device = "ps5mic", .flow = eCapture, .fmt = &mix };
        call(get_mix_format, &params);
        assert(params.result == S_OK && mix.Format.nChannels == 1 && mix.Format.nSamplesPerSec == 48000);
        assert(mix.dwChannelMask == KSAUDIO_SPEAKER_MONO);
        assert(IsEqualGUID(&mix.SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        struct is_format_supported_params supported = { .device = "ps5mic", .flow = eCapture,
                                                         .share = AUDCLNT_SHAREMODE_SHARED, .fmt_in = &fmt };
        call(is_format_supported, &supported);
        assert(supported.result == S_OK);
        /* the render device is no microphone */
        supported.device = "ps5";
        call(is_format_supported, &supported);
        assert(supported.result == AUDCLNT_E_DEVICE_INVALIDATED);
    }

    /* A microphone the title cannot open is an invalid device. */
    mic_refuse = 1;
    create(&fmt, &result);
    assert(result == AUDCLNT_E_DEVICE_INVALIDATED);
    mic_refuse = 0;

    /* 16-bit mono at the microphone's rate: the ramp comes through as is. */
    stream = create(&fmt, &result);
    assert(result == S_OK);
    {
        struct set_event_handle_params params = { .stream = stream, .event = (HANDLE)&events };
        call(set_event_handle, &params);
        assert(params.result == S_OK);
    }
    assert(release(stream, 160) == AUDCLNT_E_OUT_OF_ORDER);
    assert(packet(stream, &data, &frames, &flags, &devpos) == AUDCLNT_S_BUFFER_EMPTY);  /* not started */
    {
        struct start_params params = { .stream = stream };
        call(start, &params);
        assert(params.result == S_OK);
    }

    assert(packet(stream, &data, &frames, &flags, &devpos) == S_OK);
    assert(frames == 160 && flags == 0 && devpos == 0);
    {
        const int16_t *s = (const int16_t *)data;
        /* the converter's one-sample history, then the ramp */
        assert(s[0] == 0 && s[1] == 1 && s[159] == 159);
    }
    assert(packet(stream, &data, &frames, &flags, &devpos) == AUDCLNT_E_OUT_OF_ORDER);
    assert(release(stream, 100) == AUDCLNT_E_INVALID_SIZE);
    assert(release(stream, 160) == S_OK);
    last_devpos = devpos;
    assert(packet(stream, &data, &frames, &flags, &devpos) == S_OK);
    assert(devpos == last_devpos + 160 && ((const int16_t *)data)[0] == 160);
    assert(release(stream, 0) == S_OK);   /* nothing read: the packet stays */
    assert(packet(stream, &data, &frames, &flags, &devpos) == S_OK && devpos == last_devpos + 160);
    assert(release(stream, 160) == S_OK);
    assert(__atomic_load_n(&events, __ATOMIC_RELAXED) > 0);
    assert(!muted_property());

    /* The mute button: zeros, flagged silent, and the property says so. */
    __atomic_store_n(&mic_now, PW_WINE_MIC_MUTED, __ATOMIC_RELAXED);
    assert(muted_property());
    {
        /* drain what was captured before the mute, then a whole packet after it */
        struct get_current_padding_params params = { .stream = stream, .padding = &padding };
        int silent = 0;
        usleep(20000);
        for (int i = 0; i < 200 && !silent; i++)
        {
            assert(packet(stream, &data, &frames, &flags, &devpos) == S_OK);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                for (UINT32 f = 0; f < frames; f++) assert(((const int16_t *)data)[f] == 0);
                silent = 1;
            }
            assert(release(stream, frames) == S_OK);
        }
        assert(silent);
        call(get_current_padding, &params);
        assert(params.result == S_OK);
    }
    /* unmuted: sound again, not flagged */
    __atomic_store_n(&mic_now, PW_WINE_MIC_LIVE, __ATOMIC_RELAXED);
    assert(!muted_property());
    {
        int live = 0;
        usleep(20000);
        for (int i = 0; i < 200 && !live; i++)
        {
            assert(packet(stream, &data, &frames, &flags, &devpos) == S_OK);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                const int16_t *s = (const int16_t *)data;
                live = s[frames - 1] != 0;
            }
            assert(release(stream, frames) == S_OK);
        }
        assert(live);
    }

    /* A reader that falls behind loses the oldest frames, and is told. */
    usleep(300000);     /* 300 ms of the microphone into a 100 ms ring */
    assert(packet(stream, &data, &frames, &flags, &devpos) == S_OK);
    assert(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
    assert(release(stream, frames) == S_OK);
    assert(packet(stream, &data, &frames, &flags, &devpos) == S_OK);
    assert(!(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY));
    assert(release(stream, frames) == S_OK);

    /* The position counts every frame the microphone gave the stream. */
    {
        UINT64 pos = 0;
        struct get_position_params params = { .stream = stream, .pos = &pos };
        call(get_position, &params);
        assert(params.result == S_OK && pos / 2 >= devpos + frames);
    }

    /* Stopped and reset: nothing held, and capture carries on afterwards. */
    {
        struct stop_params stop_params = { .stream = stream };
        struct reset_params reset_params = { .stream = stream };
        struct get_current_padding_params padding_params = { .stream = stream, .padding = &padding };
        call(stop, &stop_params);
        assert(stop_params.result == S_OK);
        call(reset, &reset_params);
        assert(reset_params.result == S_OK);
        call(get_current_padding, &padding_params);
        assert(padding == 0);
    }

    /* A 48 kHz float stereo stream: three frames a sample, on both sides. */
    {
        WAVEFORMATEX f32 = { WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0 };
        stream_handle wide = create(&f32, &result);
        struct start_params params = { .stream = wide };
        struct set_event_handle_params event = { .stream = wide, .event = (HANDLE)&events };
        assert(result == S_OK);
        /* an event-driven stream starts only with its event */
        call(start, &params);
        assert(params.result == AUDCLNT_E_EVENTHANDLE_NOT_SET);
        call(set_event_handle, &event);
        call(start, &params);
        assert(params.result == S_OK);
        assert(packet(wide, &data, &frames, &flags, &devpos) == S_OK && frames == 480);
        const float *s = (const float *)data;
        for (UINT32 f = 0; f < frames; f++) assert(s[2 * f] == s[2 * f + 1]);
        assert(s[2 * 479] > 0);
        assert(release(wide, frames) == S_OK);
        struct release_stream_params gone = { .stream = wide };
        call(release_stream, &gone);
        assert(gone.result == S_OK);
    }
    {
        struct release_stream_params params = { .stream = stream };
        call(release_stream, &params);
        assert(params.result == S_OK);
    }

    /* Render's properties are still not the driver's. */
    {
        static const PROPERTYKEY key = WINE_PKEY_AUDIOENDPOINT_DEVICE_MUTE;
        PROPVARIANT value;
        struct get_prop_value_params params = { .device = "ps5", .flow = eRender, .prop = &key, .value = &value };
        call(get_prop_value, &params);
        assert(params.result == E_NOTIMPL);
    }
    printf("wineps5 capture passed\n");
    return 0;
}
