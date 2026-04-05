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

static char *get_trigger_fifo_path(void) {
    return strdup("/tmp/voiceinput-trigger");
}

// ============================================================================
// Recording and Processing
// ============================================================================

static int process_recording(vi_ctx_t *ctx) {
    // Get audio data from ring buffer
    float *audio_data = NULL;
    size_t audio_len = 0;

    if (vi_audio_get_chunk(&ctx->audio, &audio_data, &audio_len) < 0) {
        fprintf(stderr, "Failed to get audio chunk\n");
        return -1;
    }

    if (!audio_data || audio_len == 0) {
        fprintf(stderr, "No audio data captured\n");
        free(audio_data);
        return -1;
    }

    printf("Processing %zu samples of audio data...\n", audio_len);

    char *transcribed = NULL;
    size_t transcribed_len = 0;

    // Transcribe with Gemini
    if (vi_gemini_transcribe(&ctx->gemini, audio_data, audio_len,
                             &transcribed, &transcribed_len) < 0) {
        fprintf(stderr, "Gemini transcription failed\n");
        free(audio_data);
        return -1;
    }

    free(audio_data);

    if (!transcribed || transcribed_len == 0) {
        fprintf(stderr, "No transcription result\n");
        return -1;
    }

    printf("Transcribed: %s\n", transcribed);

    // Apply AI refinement
    if (ctx->config.refinement_enabled && ctx->config.auto_punctuate) {
        char *refined = NULL;
        size_t refined_len = 0;
        if (vi_gemini_refine(&ctx->gemini, transcribed, &refined, &refined_len) == 0 && refined) {
            free(transcribed);
            transcribed = refined;
            transcribed_len = refined_len;
            printf("Refined: %s\n", transcribed);
        }
    }

    // Rule-based processing
    if (ctx->config.remove_fillers) {
        vi_textproc_remove_fillers(transcribed);
    }
    vi_textproc_trim_whitespace(transcribed);

    // Save to history
    if (ctx->config.history_enabled) {
        vi_history_add(&ctx->history, transcribed);
    }

    // Inject text
    printf("Injecting text...\n");
    if (vi_inject_text(&ctx->injector, transcribed) < 0) {
        fprintf(stderr, "Text injection failed\n");
        free(transcribed);
        return -1;
    }

    free(transcribed);
    return 0;
}

// ============================================================================
// Trigger FIFO Setup
// ============================================================================

static int setup_trigger_fifo(void) {
    char *fifo_path = get_trigger_fifo_path();
    if (!fifo_path) return -1;

    // Remove existing FIFO if any
    unlink(fifo_path);

    // Create new FIFO
    if (mkfifo(fifo_path, 0666) < 0) {
        fprintf(stderr, "Failed to create trigger FIFO: %s\n", strerror(errno));
        free(fifo_path);
        return -1;
    }

    // Open FIFO in non-blocking read mode
    g_trigger_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (g_trigger_fd < 0) {
        fprintf(stderr, "Failed to open trigger FIFO: %s\n", strerror(errno));
        unlink(fifo_path);
        free(fifo_path);
        return -1;
    }

    free(fifo_path);
    return 0;
}

static int check_trigger(void) {
    if (g_trigger_fd < 0) return 0;

    char buf[64];
    ssize_t n = read(g_trigger_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, "trigger") != NULL) {
            return 1;
        }
    } else if (n == 0) {
        // EOF — all writers closed. Reopen FIFO to accept new writers.
        close(g_trigger_fd);
        g_trigger_fd = open("/tmp/voiceinput-trigger", O_RDONLY | O_NONBLOCK);
    }

    return 0;
}

static void cleanup_trigger_fifo(void) {
    if (g_trigger_fd >= 0) {
        close(g_trigger_fd);
        g_trigger_fd = -1;
    }

    char *fifo_path = get_trigger_fifo_path();
    if (fifo_path) {
        unlink(fifo_path);
        free(fifo_path);
    }
}

// ============================================================================
// Main Event Loop
// ============================================================================

static void run_event_loop(vi_ctx_t *ctx) {
    printf("VoiceInput started.\n");
    printf("Trigger: write 'trigger' to /tmp/voiceinput-trigger or use Niri keybinding\n");
    printf("Press Ctrl+C to exit.\n\n");

    while (g_running) {
        int triggered = 0;

        // Check for trigger signal (FIFO or hotkey)
        if (check_trigger()) {
            triggered = 1;
            printf("Trigger signal received, starting recording...\n");
        } else if (vi_hotkey_check(&ctx->hotkey)) {
            triggered = 1;
            printf("Hotkey pressed, starting recording...\n");
        }

        if (triggered) {
            // Start recording
            ctx->state = VI_STATE_RECORDING;
            vi_audio_start(&ctx->audio);

            // Record for fixed duration (configurable)
            usleep(ctx->config.chunk_duration_ms * 1000);

            // Stop recording
            vi_audio_stop(&ctx->audio);
            ctx->state = VI_STATE_PROCESSING;

            printf("Recording stopped, processing...\n");

            // Process the recording
            if (process_recording(ctx) == 0) {
                printf("Done!\n\n");
            } else {
                fprintf(stderr, "Processing failed\n\n");
            }

            ctx->state = VI_STATE_IDLE;
        }

        // Small sleep to prevent CPU spinning
        usleep(50000);  // 50ms
    }
}

// ============================================================================
// Initialization and Cleanup
// ============================================================================

static int init_context(vi_ctx_t *ctx) {
    memset(ctx, 0, sizeof(vi_ctx_t));

    // Load configuration
    printf("Loading configuration...\n");
    if (vi_config_init(&ctx->config) < 0) {
        fprintf(stderr, "Failed to load config, using defaults\n");
        vi_config_defaults(&ctx->config);
    }

    // Initialize Gemini
    printf("Provider: Gemini\n");
    if (strlen(ctx->config.gemini_api_key) == 0) {
        fprintf(stderr, "WARNING: Gemini API key not set!\n");
        fprintf(stderr, "Set it in ~/.config/voiceinput/config.json\n");
    }
    if (vi_gemini_init(&ctx->gemini, ctx->config.gemini_api_key) < 0) {
        fprintf(stderr, "Failed to initialize Gemini client\n");
        return -1;
    }

    // Initialize audio capture
    printf("Initializing audio (sample_rate=%d, channels=%d)...\n",
           ctx->config.sample_rate, ctx->config.channels);
    if (vi_audio_init(&ctx->audio, ctx->config.sample_rate,
                      ctx->config.channels) < 0) {
        fprintf(stderr, "Failed to initialize audio capture\n");
        return -1;
    }

    // Initialize hotkey listener (optional, for fallback)
    printf("Initializing hotkey listener (%s+%s)...\n",
           ctx->config.hotkey_modifier, ctx->config.hotkey_key);
    vi_hotkey_init(&ctx->hotkey, ctx->config.hotkey_modifier,
                   ctx->config.hotkey_key);

    // Initialize text injector
    printf("Initializing text injector (method=%d)...\n",
           ctx->config.injection_method);
    if (vi_inject_init(&ctx->injector, ctx->config.injection_method) < 0) {
        fprintf(stderr, "Warning: Text injector initialization failed\n");
        // Non-fatal, we can still run
    }

    // Initialize history
    if (ctx->config.history_enabled) {
        char *history_path = get_history_path();
        if (history_path) {
            printf("Initializing history database (%s)...\n", history_path);
            if (vi_history_init(&ctx->history, history_path,
                               ctx->config.history_max_entries) < 0) {
                fprintf(stderr, "Warning: Failed to initialize history\n");
                ctx->config.history_enabled = false;
            }
            free(history_path);
        }
    }

    // Setup trigger FIFO
    printf("Setting up trigger FIFO...\n");
    if (setup_trigger_fifo() < 0) {
        fprintf(stderr, "Warning: Failed to setup trigger FIFO\n");
        // Non-fatal
    }

    ctx->state = VI_STATE_IDLE;
    return 0;
}

static void cleanup_context(vi_ctx_t *ctx) {
    cleanup_trigger_fifo();
    vi_history_cleanup(&ctx->history);
    vi_inject_cleanup(&ctx->injector);
    vi_hotkey_cleanup(&ctx->hotkey);

    vi_gemini_cleanup(&ctx->gemini);
    vi_audio_cleanup(&ctx->audio);

    if (ctx->current_text) {
        free(ctx->current_text);
    }
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("VoiceInput v%d.%d.%d\n", VI_VERSION_MAJOR, VI_VERSION_MINOR,
           VI_VERSION_PATCH);
    printf("https://github.com/antony/voiceinput\n\n");

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize libcurl globally (once, before any Gemini calls)
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Create config directory if it doesn't exist
    char *config_dir = get_config_dir();
    if (config_dir) {
        mkdir(config_dir, 0755);
        free(config_dir);
    }

    // Initialize context
    if (init_context(&g_ctx) < 0) {
        fprintf(stderr, "Failed to initialize\n");
        return 1;
    }

    // Run main event loop
    run_event_loop(&g_ctx);

    // Cleanup
    printf("\nShutting down...\n");
    cleanup_context(&g_ctx);
    curl_global_cleanup();

    return 0;
}
