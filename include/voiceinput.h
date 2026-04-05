#ifndef VOICEINPUT_H
#define VOICEINPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

// ============================================================================
// Constants
// ============================================================================

#define VI_VERSION_MAJOR 0
#define VI_VERSION_MINOR 1
#define VI_VERSION_PATCH 0

#define VI_MAX_PATH 512
#define VI_MAX_HOTKEY_NAME 64
#define VI_MAX_MODEL_NAME 128
#define VI_MAX_URL_LEN 512
#define VI_MAX_API_KEY_LEN 256

#define VI_DEFAULT_SAMPLE_RATE 16000
#define VI_DEFAULT_CHANNELS 1
#define VI_DEFAULT_CHUNK_MS 1000

#define VI_DEFAULT_HOTKEY_MODIFIER "Ctrl"
#define VI_DEFAULT_HOTKEY_KEY "Space"

#define VI_DEFAULT_TRANSCRIPTION_MODEL "whisper-tiny"
#define VI_DEFAULT_PROCESSING_MODEL "gpt-oss:cloud"

#define VI_CONFIG_DIR ".config/voiceinput"
#define VI_CONFIG_FILE "config.json"
#define VI_HISTORY_FILE "history.db"

// ============================================================================
// Type Definitions
// ============================================================================

// Application state
typedef enum {
    VI_STATE_IDLE,
    VI_STATE_RECORDING,
    VI_STATE_PROCESSING,
    VI_STATE_INJECTING
} vi_state_t;

// Text injection method
typedef enum {
    VI_INJECT_CLIPBOARD,  // Safe: copy to clipboard + paste
    VI_INJECT_LIBEI,
    VI_INJECT_WTYPE,
    VI_INJECT_YDOTOOL
} vi_inject_method_t;

// Provider selection
typedef enum {
    VI_PROVIDER_GEMINI
} vi_provider_t;

// Configuration structure
typedef struct {
    // Provider selection
    vi_provider_t provider;

    // Gemini settings
    char gemini_api_key[VI_MAX_API_KEY_LEN];

    // Hotkey settings
    char hotkey_modifier[VI_MAX_HOTKEY_NAME];
    char hotkey_key[VI_MAX_HOTKEY_NAME];

    // Audio settings
    int sample_rate;
    int channels;
    int chunk_duration_ms;

    // Text injection
    vi_inject_method_t injection_method;
    bool injection_fallback;

    // Refinement settings
    bool refinement_enabled;
    bool remove_fillers;
    bool auto_punctuate;

    // History settings
    bool history_enabled;
    int history_max_entries;
} vi_config_t;

// Audio buffer (ring buffer for streaming)
typedef struct {
    float *data;
    size_t capacity;
    size_t size;
    size_t read_pos;
    size_t write_pos;
} vi_ring_buffer_t;

// Audio capture context
typedef struct {
    void *portaudio_stream;
    vi_ring_buffer_t *buffer;
    volatile sig_atomic_t is_recording;  // volatile for PortAudio callback thread safety
    int sample_rate;
    int channels;
} vi_audio_ctx_t;

// Ollama client context
typedef struct {
    char host[VI_MAX_URL_LEN];
    char transcription_model[VI_MAX_MODEL_NAME];
    char processing_model[VI_MAX_MODEL_NAME];
} vi_ollama_ctx_t;  // DEPRECATED - kept for build compatibility

// Gemini client context
typedef struct {
    char api_key[VI_MAX_API_KEY_LEN];
    void *curl_handle;
} vi_gemini_ctx_t;

// Hotkey listener context
typedef struct {
    void *libinput_ctx;
    void *libinput_device;
    bool is_pressed;
    char modifier[VI_MAX_HOTKEY_NAME];
    char key[VI_MAX_HOTKEY_NAME];
    // Internal key tracking state
    int modifier_code;
    int key_code;
    int modifier_pressed;
    int triggered;
} vi_hotkey_ctx_t;

// Text injector context
struct libei_inject;
typedef struct {
    vi_inject_method_t method;
    bool fallback_enabled;
    struct libei_inject *libei_context;
} vi_inject_ctx_t;

// History database context
typedef struct {
    void *sqlite_db;
    bool enabled;
    int max_entries;
} vi_history_ctx_t;

// Main application context
typedef struct {
    vi_state_t state;
    vi_config_t config;
    vi_audio_ctx_t audio;
    vi_gemini_ctx_t gemini;
    vi_hotkey_ctx_t hotkey;
    vi_inject_ctx_t injector;
    vi_history_ctx_t history;

    // Current transcription
    char *current_text;
    size_t current_text_len;
} vi_ctx_t;

// ============================================================================
// Function Declarations (Module APIs)
// ============================================================================

// config.c
int vi_config_init(vi_config_t *config);
int vi_config_load(const char *path, vi_config_t *config);
int vi_config_save(const vi_config_t *config, const char *path);
void vi_config_defaults(vi_config_t *config);

// audio.c
int vi_audio_init(vi_audio_ctx_t *ctx, int sample_rate, int channels);
int vi_audio_start(vi_audio_ctx_t *ctx);
int vi_audio_stop(vi_audio_ctx_t *ctx);
void vi_audio_cleanup(vi_audio_ctx_t *ctx);
int vi_audio_get_chunk(vi_audio_ctx_t *ctx, float **data, size_t *len);

// gemini.c
int vi_gemini_init(vi_gemini_ctx_t *ctx, const char *api_key);
int vi_gemini_transcribe(vi_gemini_ctx_t *ctx, const float *audio,
                         size_t len, char **text, size_t *text_len);
int vi_gemini_refine(vi_gemini_ctx_t *ctx, const char *input,
                     char **output, size_t *output_len);
void vi_gemini_cleanup(vi_gemini_ctx_t *ctx);

// hotkey.c
int vi_hotkey_init(vi_hotkey_ctx_t *ctx, const char *modifier, const char *key);
int vi_hotkey_check(vi_hotkey_ctx_t *ctx);
void vi_hotkey_cleanup(vi_hotkey_ctx_t *ctx);

// inject.c
int vi_inject_init(vi_inject_ctx_t *ctx, vi_inject_method_t method);
int vi_inject_text(vi_inject_ctx_t *ctx, const char *text);
void vi_inject_cleanup(vi_inject_ctx_t *ctx);

// textproc.c
int vi_textproc_remove_fillers(char *text);
int vi_textproc_trim_whitespace(char *text);
int vi_textproc_auto_punctuate(char **text);

// history.c
int vi_history_init(vi_history_ctx_t *ctx, const char *db_path,
                    int max_entries);
int vi_history_add(vi_history_ctx_t *ctx, const char *text);
int vi_history_get(vi_history_ctx_t *ctx, int index, char **text);
void vi_history_cleanup(vi_history_ctx_t *ctx);

// ring buffer utilities
vi_ring_buffer_t *vi_ring_buffer_create(size_t capacity);
void vi_ring_buffer_destroy(vi_ring_buffer_t *buf);
int vi_ring_buffer_push(vi_ring_buffer_t *buf, const float *data, size_t len);
int vi_ring_buffer_pop(vi_ring_buffer_t *buf, float *data, size_t len);
size_t vi_ring_buffer_size(vi_ring_buffer_t *buf);

#endif // VOICEINPUT_H
