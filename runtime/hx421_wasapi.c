/* ============================================================
 *  hx421_wasapi.c — Windows shared-mode WASAPI sink (DLL only)
 *
 *  Ported near-verbatim from microgarbage/src/audio/audio_sink_wasapi.c.
 *  Rationale (same as mgapi): routing HX-421 audio through bsnes-plus's Qt
 *  audio pipeline couples our audio to bsnes's emulation pacing and stutters.
 *  Instead the DLL opens its OWN WASAPI device and a worker thread renders the
 *  mixer paced by the device clock (see hx421_runtime.c); bsnes gets silence.
 *
 *  Shared-mode, event-driven: Initialize at the caller's rate with
 *  AUTOCONVERTPCM (the Audio Engine resamples to the device mix format), an
 *  auto-reset event fired each period, and wasapi_write blocks on that event
 *  for pacing (the device clock IS the audio clock).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#if defined(_WIN32)

#define INITGUID       /* instantiate the GUIDs here (no -luuid needed) */
#define COBJMACROS     /* C-style IFoo_Method() helpers */

#include "audio/sink.h"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

typedef struct {
    IAudioClient       *client;
    IAudioRenderClient *render;
    HANDLE              evt;
    UINT32              buffer_frames;
    UINT32              channels;
    bool               started;
    bool               com_pair;      /* CoUninitialize on close */
} WasapiCtx;

/* Query the default render device's NATIVE mix rate (usually 48000). We open the
 * sink at this rate and resample 44100->native ourselves, rather than handing
 * Windows a 44.1 kHz stream and trusting AUTOCONVERTPCM's SRC (mgapi's v1.99
 * lesson: the engine's rate conversion is not a reliable real-time clock).
 * Returns 0 if it can't be determined. */
uint32_t hx421_wasapi_device_rate(void) {
    uint32_t rate = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool pair = SUCCEEDED(hr);
    IMMDeviceEnumerator *en = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &IID_IMMDeviceEnumerator, (void **)&en))) {
        IMMDevice *dev = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
            IAudioClient *cl = NULL;
            if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&cl))) {
                WAVEFORMATEX *mix = NULL;
                if (SUCCEEDED(IAudioClient_GetMixFormat(cl, &mix)) && mix) {
                    rate = (uint32_t)mix->nSamplesPerSec;
                    CoTaskMemFree(mix);
                }
                IAudioClient_Release(cl);
            }
            IMMDevice_Release(dev);
        }
        IMMDeviceEnumerator_Release(en);
    }
    if (pair) CoUninitialize();
    return rate;
}

static void *wasapi_open(uint32_t sample_rate) {
    HRESULT hr;
    WasapiCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    c->com_pair = SUCCEEDED(hr);

    IMMDeviceEnumerator *enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) goto fail;

    IMMDevice *device = NULL;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(hr)) goto fail;

    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&c->client);
    IMMDevice_Release(device);
    if (FAILED(hr)) goto fail;

    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = AUDIO_SINK_CHANNELS;
    wf.nSamplesPerSec  = sample_rate;
    wf.wBitsPerSample  = AUDIO_SINK_BITS;
    wf.nBlockAlign     = (WORD)(wf.nChannels * (wf.wBitsPerSample / 8));
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize          = 0;
    c->channels = wf.nChannels;

    REFERENCE_TIME buffer_duration = (REFERENCE_TIME)40 * 10000;    /* 40 ms (was 100) */
    hr = IAudioClient_Initialize(c->client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                   | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                   | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 buffer_duration, 0, &wf, NULL);
    if (FAILED(hr)) goto fail;

    c->evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!c->evt) goto fail;
    hr = IAudioClient_SetEventHandle(c->client, c->evt);
    if (FAILED(hr)) goto fail;

    hr = IAudioClient_GetBufferSize(c->client, &c->buffer_frames);
    if (FAILED(hr)) goto fail;
    fprintf(stderr, "hx421 wasapi: device buffer = %u frames (%u ms @ %u Hz)\n",
            (unsigned)c->buffer_frames, (unsigned)(c->buffer_frames * 1000u / sample_rate),
            (unsigned)sample_rate);
    fflush(stderr);

    hr = IAudioClient_GetService(c->client, &IID_IAudioRenderClient, (void **)&c->render);
    if (FAILED(hr)) goto fail;

    BYTE *data = NULL;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(c->render, c->buffer_frames, &data)) && data) {
        memset(data, 0, (size_t)c->buffer_frames * c->channels * (AUDIO_SINK_BITS / 8));
        IAudioRenderClient_ReleaseBuffer(c->render, c->buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    hr = IAudioClient_Start(c->client);
    if (FAILED(hr)) goto fail;
    c->started = true;
    return c;

fail:
    if (c->render) IAudioRenderClient_Release(c->render);
    if (c->client) IAudioClient_Release(c->client);
    if (c->evt)    CloseHandle(c->evt);
    if (c->com_pair) CoUninitialize();
    free(c);
    return NULL;
}

static int wasapi_write(void *ctx, const int16_t *interleaved, uint32_t frames) {
    WasapiCtx *c = (WasapiCtx *)ctx;
    if (!c) return -1;
    uint32_t done = 0;
    while (done < frames) {
        DWORD wr = WaitForSingleObject(c->evt, 200);      /* pace on the device period */
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) return -1;

        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(c->client, &padding))) return -1;
        UINT32 available = c->buffer_frames - padding;
        if (available == 0) continue;

        UINT32 chunk = frames - done;
        if (chunk > available) chunk = available;

        BYTE *data = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(c->render, chunk, &data)) || !data) return -1;
        const size_t bpf = (size_t)c->channels * (AUDIO_SINK_BITS / 8);
        memcpy(data, interleaved + (size_t)done * c->channels, (size_t)chunk * bpf);
        IAudioRenderClient_ReleaseBuffer(c->render, chunk, 0);
        done += chunk;
    }
    return (int)done;
}

static void wasapi_close(void *ctx) {
    WasapiCtx *c = (WasapiCtx *)ctx;
    if (!c) return;
    if (c->started) IAudioClient_Stop(c->client);
    if (c->render)  IAudioRenderClient_Release(c->render);
    if (c->client)  IAudioClient_Release(c->client);
    if (c->evt)     CloseHandle(c->evt);
    if (c->com_pair) CoUninitialize();
    free(c);
}

const AudioSinkBackend audio_sink_wasapi = {
    "wasapi", wasapi_open, wasapi_write, wasapi_close
};

#endif /* _WIN32 */
