#include "voiceinput.h"
#include <portaudio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>

// Mutex protecting ring buffer fields (size, read_pos, write_pos, data)
static pthread_mutex_t g_ring_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================================
// Ring Buffer Implementation
// ============================================================================

vi_ring_buffer_t *vi_ring_buffer_create(size_t capacity) {
    vi_ring_buffer_t *buf = calloc(1, sizeof(vi_ring_buffer_t));
    if (!buf) return NULL;

    buf->data = calloc(capacity, sizeof(float));
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->capacity = capacity;
    buf->size = 0;
    buf->read_pos = 0;
    buf->write_pos = 0;

    return buf;
}

void vi_ring_buffer_destroy(vi_ring_buffer_t *buf) {
    if (!buf) return;
    free(buf->data);
    free(buf);
}

int vi_ring_buffer_push(vi_ring_buffer_t *buf, const float *data, size_t len) {
    if (!buf || !data) return -1;

    pthread_mutex_lock(&g_ring_mutex);
    for (size_t i = 0; i < len; i++) {
        if (buf->size >= buf->capacity) {
            buf->read_pos = (buf->read_pos + 1) % buf->capacity;
            buf->size--;
        }

        buf->data[buf->write_pos] = data[i];
        buf->write_pos = (buf->write_pos + 1) % buf->capacity;
        buf->size++;
    }
    pthread_mutex_unlock(&g_ring_mutex);

    return 0;
}

int vi_ring_buffer_pop(vi_ring_buffer_t *buf, float *data, size_t len) {
    if (!buf || !data) return -1;

    pthread_mutex_lock(&g_ring_mutex);
    size_t to_read = len < buf->size ? len : buf->size;

    for (size_t i = 0; i < to_read; i++) {
        data[i] = buf->data[buf->read_pos];
        buf->read_pos = (buf->read_pos + 1) % buf->capacity;
    }

    buf->size -= to_read;
    pthread_mutex_unlock(&g_ring_mutex);
    return (int)to_read;
}

size_t vi_ring_buffer_size(vi_ring_buffer_t *buf) {
    if (!buf) return 0;
    pthread_mutex_lock(&g_ring_mutex);
    size_t s = buf->size;
    pthread_mutex_unlock(&g_ring_mutex);
    return s;
}

// ============================================================================
// Audio Capture Implementation
// ============================================================================

static int audio_callback(const void *input, void *output,
                          unsigned long frame_count,
                          const PaStreamCallbackTimeInfo *time_info,
                          PaStreamCallbackFlags status_flags,
                          void *user_data) {
    (void)output; (void)time_info; (void)status_flags;
    vi_audio_ctx_t *ctx = (vi_audio_ctx_t *)user_data;
    const float *input_data = (const float *)input;

    if (!ctx->is_recording) {
        return paContinue;
    }

    size_t n = frame_count * ctx->channels;

    // VAD: compute RMS energy to detect speech
    float energy = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float s = input_data[i];
        energy += s * s;
    }
    energy = sqrtf(energy / (float)n);

    // Smooth energy (EMA)
    ctx->vad_energy = ctx->vad_energy * 0.7f + energy * 0.3f;

    // Threshold for speech detection (mic hiss is typically < 0.01 RMS)
    // If energy exceeds threshold, VAD is triggered
    if (!ctx->vad_triggered && ctx->vad_energy > 0.01f) {
        ctx->vad_triggered = 1;
    }

    // Only store audio once VAD has triggered (speech detected)
    // This prevents silence from filling the buffer and overwriting
    // the beginning of the utterance
    if (ctx->vad_triggered) {
        vi_ring_buffer_push(ctx->buffer, input_data, n);
    }

    return paContinue;
}

int vi_audio_init(vi_audio_ctx_t *ctx, int sample_rate, int channels) {
    if (!ctx) return -1;

    memset(ctx, 0, sizeof(vi_audio_ctx_t));
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;

    // Initialize PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio initialization failed: %s\n", Pa_GetErrorText(err));
        return -1;
    }

    // Create ring buffer (60 seconds capacity)
    size_t buffer_capacity = sample_rate * channels * 60;
    ctx->buffer = vi_ring_buffer_create(buffer_capacity);
    if (!ctx->buffer) {
        Pa_Terminate();
        return -1;
    }

    // Get default input device (microphone, NOT system audio monitor)
    PaDeviceIndex device = Pa_GetDefaultInputDevice();
    if (device == paNoDevice) {
        fprintf(stderr, "No default input device found\n");
        vi_audio_cleanup(ctx);
        return -1;
    }

    // Verify this is not a monitor device (system audio loopback)
    const PaDeviceInfo *device_info = Pa_GetDeviceInfo(device);
    if (device_info && strstr(device_info->name, "monitor")) {
        fprintf(stderr, "Warning: Default device is a monitor, searching for mic...\n");
        // Find a non-monitor device
        int num_devices = Pa_GetDeviceCount();
        for (int i = 0; i < num_devices; i++) {
            const PaDeviceInfo *di = Pa_GetDeviceInfo(i);
            if (di && di->maxInputChannels > 0 && !strstr(di->name, "monitor")) {
                device = i;
                device_info = di;
                fprintf(stderr, "Using device %d: %s\n", device, di->name);
                break;
            }
        }
    }

    if (device == paNoDevice || device_info->maxInputChannels == 0) {
        fprintf(stderr, "No microphone input device found\n");
        vi_audio_cleanup(ctx);
        return -1;
    }

    // Configure input parameters
    PaStreamParameters params;
    params.device = device;
    params.channelCount = channels;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = device_info->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = NULL;
    const PaStreamParameters *input_params = &params;

    // Open stream
    err = Pa_OpenStream(
        &ctx->portaudio_stream,
        input_params,
        NULL,  // No output
        sample_rate,
        256,   // Frames per buffer
        paClipOff,
        audio_callback,
        ctx
    );

    if (err != paNoError) {
        fprintf(stderr, "Failed to open audio stream: %s\n", Pa_GetErrorText(err));
        vi_audio_cleanup(ctx);
        return -1;
    }

    // Start the stream immediately and keep it running permanently.
    // The callback returns paContinue without storing data when
    // is_recording is false, so there is no CPU cost while idle.
    // This eliminates Pa_StartStream latency when recording begins.
    err = Pa_StartStream(ctx->portaudio_stream);
    if (err != paNoError) {
        fprintf(stderr, "Failed to start audio stream: %s\n", Pa_GetErrorText(err));
        vi_audio_cleanup(ctx);
        return -1;
    }

    return 0;
}

int vi_audio_start(vi_audio_ctx_t *ctx) {
    if (!ctx || !ctx->portaudio_stream) return -1;

    // Clear ring buffer before starting new recording
    if (ctx->buffer) {
        pthread_mutex_lock(&g_ring_mutex);
        ctx->buffer->size = 0;
        ctx->buffer->read_pos = 0;
        ctx->buffer->write_pos = 0;
        pthread_mutex_unlock(&g_ring_mutex);
    }

    // Reset VAD state
    ctx->vad_triggered = 0;
    ctx->vad_energy = 0.0f;

    // Stream is already running — just flip the flag.
    // The callback will start storing audio on its next invocation,
    // which is at most one buffer period (~16ms at 256 frames/16kHz) away.
    ctx->is_recording = true;
    return 0;
}

int vi_audio_stop(vi_audio_ctx_t *ctx) {
    if (!ctx || !ctx->portaudio_stream) return -1;

    // Just clear the flag. The stream keeps running so the next
    // recording starts without Pa_StartStream latency.
    ctx->is_recording = false;
    return 0;
}

int vi_audio_get_chunk(vi_audio_ctx_t *ctx, float **data, size_t *len) {
    if (!ctx || !ctx->buffer || !data || !len) return -1;

    size_t available = vi_ring_buffer_size(ctx->buffer);
    if (available == 0) {
        *data = NULL;
        *len = 0;
        return 0;
    }

    float *linear_buf = malloc(available * sizeof(float));
    if (!linear_buf) return -1;

    // Copy data from ring buffer to linear buffer (handles wrap-around)
    vi_ring_buffer_pop(ctx->buffer, linear_buf, available);

    *data = linear_buf;
    *len = available;
    return 0;
}

void vi_audio_cleanup(vi_audio_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->portaudio_stream) {
        Pa_CloseStream(ctx->portaudio_stream);
        ctx->portaudio_stream = NULL;
    }

    if (ctx->buffer) {
        vi_ring_buffer_destroy(ctx->buffer);
        ctx->buffer = NULL;
    }

    Pa_Terminate();
    ctx->is_recording = false;
}
