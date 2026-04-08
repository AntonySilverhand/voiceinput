#include "voiceinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>
#include <curl/curl.h>

// ============================================================================
// Global State
// ============================================================================

static volatile sig_atomic_t g_running = 1;
static vi_ctx_t g_ctx;
static int g_trigger_fd = -1;
static vi_desktop_t g_desktop = VI_DESKTOP_UNKNOWN;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

static char *get_config_dir(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home) return NULL;

    char *path = malloc(VI_MAX_PATH);
    snprintf(path, VI_MAX_PATH, "%s/%s", home, VI_CONFIG_DIR);
    return path;
}

static char *get_history_path(void) {
    char *config_dir = get_config_dir();
    if (!config_dir) return NULL;
    char *path = malloc(VI_MAX_PATH);
    snprintf(path, VI_MAX_PATH, "%s/%s", config_dir, VI_HISTORY_FILE);
    free(config_dir);
    return path;
}

// ============================================================================
// Recording and Processing
// ============================================================================

static int start_recording(vi_ctx_t *ctx) {
    if (ctx->state == VI_STATE_RECORDING) return 0;

    printf("Starting recording...\n");
    ctx->state = VI_STATE_RECORDING;
    vi_indicator_set_state(VI_STATE_RECORDING);

    // Clear buffer
    if (ctx->audio.buffer) {
        ctx->audio.buffer->size = 0;
        ctx->audio.buffer->read_pos = 0;
        ctx->audio.buffer->write_pos = 0;
    }

    return vi_audio_start(&ctx->audio);
}

static int stop_recording(vi_ctx_t *ctx) {
    if (ctx->state != VI_STATE_RECORDING) return 0;

    printf("Stopping recording...\n");
    vi_audio_stop(&ctx->audio);
    ctx->state = VI_STATE_PROCESSING;
    vi_indicator_set_state(VI_STATE_PROCESSING);

    float *audio_data = NULL;
    size_t audio_len = 0;
    if (vi_audio_get_chunk(&ctx->audio, &audio_data, &audio_len) < 0 || !audio_data || audio_len == 0) {
        fprintf(stderr, "No audio data captured\n");
        free(audio_data);
        ctx->state = VI_STATE_IDLE;
        vi_indicator_set_state(VI_STATE_IDLE);
        return -1;
    }

    char *transcribed = NULL;
    size_t transcribed_len = 0;
    if (vi_gemini_transcribe(&ctx->gemini, audio_data, audio_len, &transcribed, &transcribed_len) < 0) {
        fprintf(stderr, "Transcription failed\n");
        free(audio_data);
        ctx->state = VI_STATE_IDLE;
        vi_indicator_set_state(VI_STATE_IDLE);
        return -1;
    }
    free(audio_data);

    if (ctx->config.refinement_enabled) {
        char *refined = NULL;
        size_t refined_len = 0;
        if (vi_gemini_refine(&ctx->gemini, transcribed, &refined, &refined_len) == 0 && refined) {
            free(transcribed);
            transcribed = refined;
        }
    }

    vi_textproc_trim_whitespace(transcribed);
    if (ctx->config.remove_fillers) vi_textproc_remove_fillers(transcribed);

    if (ctx->config.history_enabled) vi_history_add(&ctx->history, transcribed);

    printf("Injecting: %s\n", transcribed);
    ctx->state = VI_STATE_INJECTING;
    vi_indicator_set_state(VI_STATE_INJECTING);

    if (vi_inject_text(&ctx->injector, transcribed) < 0) {
        fprintf(stderr, "Injection failed\n");
    }

    free(transcribed);
    ctx->state = VI_STATE_IDLE;
    vi_indicator_set_state(VI_STATE_IDLE);
    return 0;
}

// ============================================================================
// Trigger FIFO Setup
// ============================================================================

static int setup_trigger_fifo(void) {
    const char *fifo_path = "/tmp/voiceinput-trigger";
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) return -1;
    g_trigger_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    return g_trigger_fd;
}

static void process_fifo_commands(vi_ctx_t *ctx) {
    if (g_trigger_fd < 0) return;
    char buf[64];
    ssize_t n = read(g_trigger_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        if (n == 0) {
            close(g_trigger_fd);
            g_trigger_fd = open("/tmp/voiceinput-trigger", O_RDONLY | O_NONBLOCK);
        }
        return;
    }
    buf[n] = '\0';

    if (strstr(buf, "start")) start_recording(ctx);
    else if (strstr(buf, "stop")) stop_recording(ctx);
    else if (strstr(buf, "trigger")) {
        if (ctx->state == VI_STATE_RECORDING) stop_recording(ctx);
        else start_recording(ctx);
    }
}

// ============================================================================
// Main Loop
// ============================================================================

int main(int argc, char *argv[]) {
    bool debug = (argc > 1 && strcmp(argv[1], "--debug") == 0);
    printf("VoiceInput Unified Daemon\n");

    g_desktop = vi_desktop_detect();
    if (debug) {
        printf("[DEBUG] Desktop: %s\n", vi_desktop_to_str(g_desktop));
        printf("[DEBUG] Wayland: %s\n", vi_is_wayland() ? "Yes" : "No");
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    char *config_dir = get_config_dir();
    if (config_dir) { mkdir(config_dir, 0755); free(config_dir); }

    if (vi_config_init(&g_ctx.config) < 0) vi_config_defaults(&g_ctx.config);
    if (vi_gemini_init(&g_ctx.gemini, g_ctx.config.gemini_api_key) < 0) return 1;
    if (vi_audio_init(&g_ctx.audio, g_ctx.config.sample_rate, g_ctx.config.channels) < 0) return 1;
    vi_hotkey_init(&g_ctx.hotkey, g_ctx.config.hotkey_modifier, g_ctx.config.hotkey_key);
    vi_inject_init(&g_ctx.injector, g_ctx.config.injection_method);

    if (g_ctx.config.history_enabled) {
        char *hp = get_history_path();
        if (hp) { vi_history_init(&g_ctx.history, hp, g_ctx.config.history_max_entries); free(hp); }
    }

    setup_trigger_fifo();
    printf("Ready. Use FIFO (/tmp/voiceinput-trigger) or Hotkey (%s+%s)\n",
           g_ctx.config.hotkey_modifier, g_ctx.config.hotkey_key);

    while (g_running) {
        process_fifo_commands(&g_ctx);

        if (vi_hotkey_check(&g_ctx.hotkey)) {
            if (g_ctx.state == VI_STATE_RECORDING) stop_recording(&g_ctx);
            else start_recording(&g_ctx);
        }

        // Handle "Hold" mode for libinput if we wanted to (omitted for brevity in this toggle version)
        // But the user's Debian+GNOME will likely use the FIFO via custom shortcut.

        usleep(10000);
    }

    vi_history_cleanup(&g_ctx.history);
    vi_inject_cleanup(&g_ctx.injector);
    vi_hotkey_cleanup(&g_ctx.hotkey);
    vi_gemini_cleanup(&g_ctx.gemini);
    vi_audio_cleanup(&g_ctx.audio);
    curl_global_cleanup();
    return 0;
}
