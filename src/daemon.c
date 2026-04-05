/**
 * VoiceInput Daemon - Press-and-Hold Recording with Visual Feedback
 *
 * Listens for Super+Space globally, records while held, shows visual feedback.
 */

#include "voiceinput.h"
#include <linux/input.h>
#include <libinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <curl/curl.h>

// ============================================================================
// Global State
// ============================================================================

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_recording = 0;
static pid_t g_voiceinput_pid = 0;
static int g_trigger_fd = -1;
static vi_audio_ctx_t g_audio;
static vi_gemini_ctx_t g_gemini;
static vi_config_t g_config;

// Visual indicator state
typedef enum {
    VISUAL_IDLE,
    VISUAL_RECORDING,
    VISUAL_PROCESSING,
    VISUAL_ERROR
} visual_state_t;

static visual_state_t g_visual_state = VISUAL_IDLE;

// ============================================================================
// Visual Feedback (using zenity for desktop notifications)
// ============================================================================

static pid_t g_notification_pid = 0;

static void set_visual_state(visual_state_t state) {
    g_visual_state = state;

    switch (state) {
        case VISUAL_RECORDING:
            // Show persistent "Recording..." notification
            g_notification_pid = fork();
            if (g_notification_pid == 0) {
                // Child process
                execlp("zenity", "zenity", "--notification",
                       "--window-icon=gtk-media-record",
                       "--text=VoiceInput: Recording... (hold Super+Space)",
                       "--timeout=60", NULL);
                exit(0);
            }
            break;

        case VISUAL_PROCESSING:
            // Kill recording notification if any
            if (g_notification_pid > 0) {
                kill(g_notification_pid, SIGTERM);
                g_notification_pid = 0;
            }
            // Show "Processing..." notification
            system("zenity --notification --window-icon=gtk-dialog-info "
                   "--text='VoiceInput: Processing...' --timeout=3 2>/dev/null &");
            break;

        case VISUAL_ERROR:
            // Kill any existing notification
            if (g_notification_pid > 0) {
                kill(g_notification_pid, SIGTERM);
                g_notification_pid = 0;
            }
            // Show error notification
            system("zenity --notification --window-icon=gtk-dialog-error "
                   "--text='VoiceInput: Error occurred' --timeout=3 2>/dev/null &");
            break;

        case VISUAL_IDLE:
        default:
            // Kill any existing notification
            if (g_notification_pid > 0) {
                kill(g_notification_pid, SIGTERM);
                g_notification_pid = 0;
            }
            break;
    }
}

// ============================================================================
// Audio Processing
// ============================================================================

static int process_audio_and_inject(vi_audio_ctx_t *audio, vi_gemini_ctx_t *gemini,
                                    vi_config_t *config) {
    float *audio_data = NULL;
    size_t audio_len = 0;

    if (vi_audio_get_chunk(audio, &audio_data, &audio_len) < 0 ||
        !audio_data || audio_len == 0) {
        free(audio_data);
        return -1;
    }

    set_visual_state(VISUAL_PROCESSING);

    char *transcribed = NULL;
    size_t transcribed_len = 0;

    // Transcribe with Gemini
    if (vi_gemini_transcribe(gemini, audio_data, audio_len,
                             &transcribed, &transcribed_len) < 0) {
        free(audio_data);
        set_visual_state(VISUAL_ERROR);
        return -1;
    }

    free(audio_data);

    if (!transcribed || transcribed_len == 0) {
        set_visual_state(VISUAL_ERROR);
        return -1;
    }

    // Refine with Gemini
    char *refined = NULL;
    size_t refined_len = 0;

    if (config->refinement_enabled &&
        vi_gemini_refine(gemini, transcribed, &refined, &refined_len) == 0 &&
        refined) {
        free(transcribed);
        transcribed = refined;
    }

    // Apply text processing
    if (config->remove_fillers) {
        vi_textproc_remove_fillers(transcribed);
    }
    vi_textproc_trim_whitespace(transcribed);

    // Inject text
    vi_inject_ctx_t injector = {0};
    vi_inject_init(&injector, config->injection_method);
    vi_inject_text(&injector, transcribed);
    vi_inject_cleanup(&injector);

    free(transcribed);
    set_visual_state(VISUAL_IDLE);

    return 0;
}

// ============================================================================
// Hotkey Handling (Press-and-Hold)
// ============================================================================

static struct {
    int super_code;
    int space_code;
    int super_pressed;
    int space_pressed;
    int recording_active;
} g_hotkey = {0};

static void on_hotkey_press(void) {
    if (g_recording || g_hotkey.recording_active) return;

    printf("Hotkey pressed - starting recording\n");
    g_recording = 1;
    g_hotkey.recording_active = 1;

    // Start audio capture
    vi_audio_start(&g_audio);
    set_visual_state(VISUAL_RECORDING);
}

static void on_hotkey_release(void) {
    if (!g_recording || !g_hotkey.recording_active) return;

    printf("Hotkey released - stopping recording\n");
    g_recording = 0;
    g_hotkey.recording_active = 0;

    // Stop audio capture
    vi_audio_stop(&g_audio);

    // Process and inject
    printf("Processing audio...\n");
    int result = process_audio_and_inject(&g_audio, &g_gemini, &g_config);
    if (result == 0) {
        printf("✅ Transcription complete!\n");
    } else {
        fprintf(stderr, "❌ Transcription failed (result=%d)\n", result);
        set_visual_state(VISUAL_ERROR);
        usleep(500000);
        set_visual_state(VISUAL_IDLE);
    }
}

static void process_hotkey_event(vi_hotkey_ctx_t *ctx) {
    if (!ctx || !ctx->libinput_ctx) return;

    libinput_dispatch(ctx->libinput_ctx);

    struct libinput_event *event;
    while ((event = libinput_get_event(ctx->libinput_ctx)) != NULL) {
        enum libinput_event_type type = libinput_event_get_type(event);

        if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            struct libinput_event_keyboard *kb_event =
                libinput_event_get_keyboard_event(event);

            uint32_t key = libinput_event_keyboard_get_key(kb_event);
            enum libinput_key_state state =
                libinput_event_keyboard_get_key_state(kb_event);

            int pressed = (state == LIBINPUT_KEY_STATE_PRESSED);

            // Debug logging
            fprintf(stderr, "[DEBUG] Key event: key=%u state=%d pressed=%d\n", key, state, pressed);

            // Track Super key
            if (key == (uint32_t)g_hotkey.super_code) {
                g_hotkey.super_pressed = pressed;
                fprintf(stderr, "[DEBUG] Super key %s\n", pressed ? "PRESSED" : "RELEASED");
            }

            // Track Space key
            if (key == (uint32_t)g_hotkey.space_code) {
                g_hotkey.space_pressed = pressed;
                fprintf(stderr, "[DEBUG] Space key %s (super=%d, recording=%d)\n",
                        pressed ? "PRESSED" : "RELEASED", g_hotkey.super_pressed, g_hotkey.recording_active);

                // Press-and-hold logic
                if (pressed && g_hotkey.super_pressed) {
                    fprintf(stderr, "[DEBUG] Triggering START recording\n");
                    on_hotkey_press();
                } else if (!pressed && g_hotkey.recording_active) {
                    fprintf(stderr, "[DEBUG] Triggering STOP recording\n");
                    on_hotkey_release();
                }
            }
        }

        libinput_event_destroy(event);
        libinput_dispatch(ctx->libinput_ctx);
    }
}

// ============================================================================
// Signal Handlers
// ============================================================================

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// ============================================================================
// Initialization
// ============================================================================

static int init_gemini(vi_gemini_ctx_t *ctx, vi_config_t *config) {
    (void)config;

    // Get API key from environment
    const char *api_key = getenv("GEMINI_API_KEY");
    if (!api_key || strlen(api_key) == 0) {
        fprintf(stderr, "❌ GEMINI_API_KEY not set in environment\n");
        fprintf(stderr, "   Add to /etc/environment: GEMINI_API_KEY=your-key\n");
        return -1;
    }

    printf("Initializing Gemini with API key: %.8s...\n", api_key);

    if (vi_gemini_init(ctx, api_key) < 0) {
        fprintf(stderr, "❌ Failed to initialize Gemini client\n");
        return -1;
    }

    // Quick validation request
    printf("Validating API key...\n");
    char *test_text = NULL;
    size_t test_len = 0;

    // Create a tiny test audio (silence)
    float test_audio[1600] = {0};  // 0.1 second of silence

    if (vi_gemini_transcribe(ctx, test_audio, 1600, &test_text, &test_len) == 0) {
        printf("✅ API key validated successfully\n");
        if (test_text) free(test_text);
    } else {
        fprintf(stderr, "⚠️  API request failed - check your key\n");
        if (test_text) free(test_text);
        // Continue anyway - might be temporary network issue
    }

    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("VoiceInput Daemon v%d.%d.%d\n", VI_VERSION_MAJOR, VI_VERSION_MINOR,
           VI_VERSION_PATCH);
    printf("Press-and-hold Super+Space to dictate\n\n");

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize libcurl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Load configuration
    printf("Loading configuration...\n");
    if (vi_config_load(NULL, &g_config) < 0) {
        fprintf(stderr, "Failed to load config, using defaults\n");
        vi_config_defaults(&g_config);
    }

    // Initialize Gemini
    printf("Initializing Gemini...\n");
    if (init_gemini(&g_gemini, &g_config) < 0) {
        fprintf(stderr, "Failed to initialize Gemini\n");
        return 1;
    }

    // Initialize audio
    printf("Initializing audio...\n");
    if (vi_audio_init(&g_audio, g_config.sample_rate, g_config.channels) < 0) {
        fprintf(stderr, "Failed to initialize audio\n");
        vi_gemini_cleanup(&g_gemini);
        return 1;
    }

    // Initialize hotkey state
    g_hotkey.super_code = KEY_LEFTMETA;  // Super/Windows key
    g_hotkey.space_code = KEY_SPACE;

    // Initialize hotkey listener
    printf("Initializing hotkey listener (Super+Space)...\n");
    vi_hotkey_ctx_t hotkey_ctx;
    if (vi_hotkey_init(&hotkey_ctx, "Super", "Space") < 0) {
        fprintf(stderr, "Failed to initialize hotkey listener\n");
        fprintf(stderr, "Make sure you have permission to access /dev/input/event*\n");
        vi_audio_cleanup(&g_audio);
        vi_gemini_cleanup(&g_gemini);
        return 1;
    }

    printf("\nDaemon running. Press-and-hold Super+Space to dictate.\n");
    printf("Press Ctrl+C to exit.\n\n");

    // Main event loop
    while (g_running) {
        process_hotkey_event(&hotkey_ctx);
        usleep(10000);  // 10ms polling
    }

    // Cleanup
    printf("\nShutting down...\n");
    vi_hotkey_cleanup(&hotkey_ctx);
    vi_audio_cleanup(&g_audio);
    vi_gemini_cleanup(&g_gemini);
    curl_global_cleanup();

    return 0;
}
