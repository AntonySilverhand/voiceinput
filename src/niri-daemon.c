/**
 * VoiceInput Daemon for Niri (Wayland)
 *
 * Uses FIFO for start/stop control from Niri keybindings
 * Records until stop is received (no time limit)
 */

#include "voiceinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <curl/curl.h>

// ============================================================================
// Global State
// ============================================================================

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_recording = 0;
static vi_audio_ctx_t g_audio;
static vi_gemini_ctx_t g_gemini;
static vi_config_t g_config;
static vi_inject_ctx_t g_injector;
static int g_fifo_fd = -1;

// ============================================================================
// Visual Feedback
// ============================================================================

static void show_notification(const char *text, const char *icon, int timeout_ms) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        // Redirect stderr to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        if (timeout_ms > 0) {
            char timeout_str[16];
            snprintf(timeout_str, sizeof(timeout_str), "%d", timeout_ms / 1000);
            execlp("zenity", "zenity", "--notification",
                   "--window-icon", icon, "--text", text,
                   "--timeout", timeout_str, NULL);
        } else {
            execlp("zenity", "zenity", "--notification",
                   "--window-icon", icon, "--text", text, NULL);
        }
        _exit(127);
    }
    // Don't wait — fire and forget
}

static void close_all_notifications(void) {
    system("pkill -f 'zenity.*notification.*VoiceInput' 2>/dev/null || true");
    system("pkill -f 'zenity.*VoiceInput' 2>/dev/null || true");
}

static void set_state(const char *state) {
    close_all_notifications();

    if (strcmp(state, "recording") == 0) {
        show_notification("VoiceInput: Recording... (release to transcribe)", "gtk-media-record", 0);
    } else if (strcmp(state, "processing") == 0) {
        show_notification("VoiceInput: Processing...", "gtk-dialog-info", 3000);
    } else if (strcmp(state, "error") == 0) {
        show_notification("VoiceInput: Error", "gtk-dialog-error", 3000);
    } else if (strcmp(state, "typing") == 0) {
        show_notification("VoiceInput: Typing...", "gtk-apply", 2000);
    }
    // idle - no notification
}

// ============================================================================
// Audio Processing
// ============================================================================

static char g_session_log_path[512] = {0};

static void start_session_log(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    struct tm *tm_info = localtime(&t);

    // Create timestamped log file: /tmp/voiceinput_YYYYMMDD_HHMMSS_XXX.log
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
    snprintf(g_session_log_path, sizeof(g_session_log_path),
             "/tmp/voiceinput_%s_%06ld.log", timestamp, (long)tv.tv_usec);

    FILE *f = fopen(g_session_log_path, "w");
    if (f) {
        fprintf(f, "=== VoiceInput Session Started ===\n");
        fprintf(f, "Timestamp: %s.%06ld\n", timestamp, (long)tv.tv_usec);
        fclose(f);
    }
    fprintf(stderr, "Session log: %s\n", g_session_log_path);
}

static void log_to_session(const char *fmt, ...) {
    if (g_session_log_path[0] == '\0') return;

    va_list args;
    va_start(args, fmt);

    FILE *f = fopen(g_session_log_path, "a");
    if (f) {
        vfprintf(f, fmt, args);
        fclose(f);
    }
    va_end(args);
}

static int process_and_inject(void) {
    float *audio_data = NULL;
    size_t audio_len = 0;

    if (vi_audio_get_chunk(&g_audio, &audio_data, &audio_len) < 0 ||
        !audio_data || audio_len == 0) {
        fprintf(stderr, "No audio data\n");
        log_to_session("ERROR: No audio data\n");
        free(audio_data);
        return -1;
    }

    float duration_sec = (float)audio_len / g_config.sample_rate;
    fprintf(stderr, "Processing %zu samples (%.1f seconds)...\n", audio_len, duration_sec);
    log_to_session("Audio: %zu samples (%.1f sec)\n", audio_len, duration_sec);

    char *transcribed = NULL;
    size_t transcribed_len = 0;

    // Transcribe with Gemini
    fprintf(stderr, "Calling Gemini API...\n");
    if (vi_gemini_transcribe(&g_gemini, audio_data, audio_len,
                             &transcribed, &transcribed_len) < 0) {
        fprintf(stderr, "Transcription failed\n");
        free(audio_data);
        set_state("error");
        return -1;
    }

    free(audio_data);

    if (!transcribed || transcribed_len == 0) {
        fprintf(stderr, "Empty transcription\n");
        set_state("error");
        log_to_session("ERROR: Empty transcription\n");
        return -1;
    }

    fprintf(stderr, "Transcribed: %s\n", transcribed);
    log_to_session("Transcribed: %s\n", transcribed);

    // AI refinement — handles self-rectification ("not X, Y"), punctuation, capitalization
    char *refined = NULL;
    size_t refined_len = 0;
    if (vi_gemini_refine(&g_gemini, transcribed, &refined, &refined_len) == 0 && refined) {
        fprintf(stderr, "Refined: %s\n", refined);
        log_to_session("Refined: %s\n", refined);
        free(transcribed);
        transcribed = refined;
    } else {
        // Fallback: just trim whitespace
        vi_textproc_trim_whitespace(transcribed);
    }

    // Inject text
    fprintf(stderr, "Injecting text...\n");
    set_state("typing");
    int ret = vi_inject_text(&g_injector, transcribed);

    if (ret < 0) {
        fprintf(stderr, "Text injection failed!\n");
        // Show the text anyway so user can copy it
        fprintf(stdout, "\n=== TEXT TO TYPE ===\n%s\n===================\n", transcribed);
    }

    free(transcribed);
    return ret;
}

// ============================================================================
// Recording Control
// ============================================================================

static int start_recording(void) {
    if (g_recording) {
        fprintf(stderr, "Already recording\n");
        return -1;
    }

    // Start new session log
    start_session_log();

    fprintf(stderr, "=== START RECORDING ===\n");
    log_to_session("=== START RECORDING ===\n");

    // CRITICAL: Clear audio buffer BEFORE starting
    if (g_audio.buffer) {
        size_t old_size = g_audio.buffer->size;
        g_audio.buffer->size = 0;
        g_audio.buffer->read_pos = 0;
        g_audio.buffer->write_pos = 0;
        memset(g_audio.buffer->data, 0, g_audio.buffer->capacity * sizeof(float));
        fprintf(stderr, "Audio buffer cleared (was %zu samples)\n", old_size);
        log_to_session("Audio buffer cleared (was %zu samples)\n", old_size);
    }

    if (vi_audio_start(&g_audio) < 0) {
        fprintf(stderr, "Failed to start audio\n");
        log_to_session("ERROR: Failed to start audio\n");
        return -1;
    }

    // Show notification immediately — VAD only records when speech is detected,
    // so silence is discarded and doesn't waste buffer space.
    g_recording = 1;
    set_state("recording");

    return 0;
}

static int stop_recording(void) {
    if (!g_recording) {
        fprintf(stderr, "Not recording\n");
        return -1;
    }

    fprintf(stderr, "=== STOP RECORDING ===\n");
    log_to_session("=== STOP RECORDING ===\n");

    vi_audio_stop(&g_audio);
    g_recording = 0;

    // PortAudio has now drained its internal buffers and the callback has finished.
    // All audio data is already in our ring buffer.

    // Get audio data BEFORE clearing buffer
    set_state("processing");
    int ret = process_and_inject();

    // Clear buffer AFTER processing (prepare for next recording)
    if (g_audio.buffer) {
        g_audio.buffer->size = 0;
        g_audio.buffer->read_pos = 0;
        g_audio.buffer->write_pos = 0;
        memset(g_audio.buffer->data, 0, g_audio.buffer->capacity * sizeof(float));
        fprintf(stderr, "Audio buffer cleared after processing\n");
        log_to_session("Audio buffer cleared\n");
    }

    set_state("idle");
    log_to_session("=== SESSION COMPLETE ===\n\n");
    return ret;
}

// ============================================================================
// FIFO Setup
// ============================================================================

static int setup_fifo(void) {
    const char *fifo_path = "/tmp/voiceinput-trigger";

    unlink(fifo_path);

    if (mkfifo(fifo_path, 0666) < 0) {
        fprintf(stderr, "Failed to create FIFO: %s\n", strerror(errno));
        return -1;
    }

    g_fifo_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (g_fifo_fd < 0) {
        fprintf(stderr, "Failed to open FIFO: %s\n", strerror(errno));
        unlink(fifo_path);
        return -1;
    }

    fprintf(stderr, "FIFO ready: %s\n", fifo_path);
    return 0;
}

static void cleanup_fifo(void) {
    if (g_fifo_fd >= 0) {
        close(g_fifo_fd);
        g_fifo_fd = -1;
    }
    unlink("/tmp/voiceinput-trigger");
}

static int process_commands(void) {
    if (g_fifo_fd < 0) return 0;

    char buf[64];
    ssize_t n = read(g_fifo_fd, buf, sizeof(buf) - 1);
    if (n == 0) {
        // EOF — all writers closed. Reopen FIFO to accept new writers.
        close(g_fifo_fd);
        g_fifo_fd = open("/tmp/voiceinput-trigger", O_RDONLY | O_NONBLOCK);
        return 0;
    }
    if (n < 0) return 0;

    buf[n] = '\0';

    fprintf(stderr, "FIFO received: %s\n", buf);

    // Check for commands - use strcmp for exact match to avoid multiple triggers
    if (strcmp(buf, "start\n") == 0 || strcmp(buf, "start") == 0) {
        start_recording();
        return 1;
    }
    if (strcmp(buf, "stop\n") == 0 || strcmp(buf, "stop") == 0) {
        stop_recording();
        return 1;
    }
    if (strcmp(buf, "trigger\n") == 0 || strcmp(buf, "trigger") == 0) {
        // Toggle: start if not recording, stop if recording
        if (g_recording) {
            stop_recording();
        } else {
            start_recording();
        }
        return 1;
    }

    return 0;
}

// ============================================================================
// Initialization
// ============================================================================

static int init_all(void) {
    // Load config
    if (vi_config_load(NULL, &g_config) < 0) {
        vi_config_defaults(&g_config);
    }

    // Get API key from environment
    const char *api_key = getenv("GEMINI_API_KEY");
    if (!api_key || strlen(api_key) == 0) {
        fprintf(stderr, "GEMINI_API_KEY not set\n");
        return -1;
    }

    fprintf(stderr, "Using API key: %.8s...\n", api_key);

    // Init Gemini
    if (vi_gemini_init(&g_gemini, api_key) < 0) {
        fprintf(stderr, "Failed to init Gemini\n");
        return -1;
    }

    // Validate API key
    fprintf(stderr, "Validating API key...\n");
    float test_audio[1600] = {0};
    char *test_text = NULL;
    size_t test_len = 0;
    if (vi_gemini_transcribe(&g_gemini, test_audio, 1600, &test_text, &test_len) == 0) {
        fprintf(stderr, "✅ API key valid\n");
        if (test_text) free(test_text);
    } else {
        fprintf(stderr, "⚠️ API validation failed, continuing anyway\n");
    }

    // Init audio
    if (vi_audio_init(&g_audio, g_config.sample_rate, g_config.channels) < 0) {
        fprintf(stderr, "Failed to init audio\n");
        return -1;
    }

    // Init text injector
    if (vi_inject_init(&g_injector, g_config.injection_method) < 0) {
        fprintf(stderr, "Warning: Text injection may not work\n");
    }

    // Setup FIFO
    return setup_fifo();
}

static void cleanup_all(void) {
    close_all_notifications();
    cleanup_fifo();
    vi_inject_cleanup(&g_injector);
    vi_audio_cleanup(&g_audio);
    vi_gemini_cleanup(&g_gemini);
}

// ============================================================================
// Signal Handlers
// ============================================================================

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("VoiceInput Daemon v%d.%d.%d (Niri/Wayland)\n",
           VI_VERSION_MAJOR, VI_VERSION_MINOR, VI_VERSION_PATCH);
    printf("\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (init_all() < 0) {
        fprintf(stderr, "Initialization failed\n");
        return 1;
    }

    printf("=== READY ===\n");
    printf("\n");
    printf("Usage with Niri keybindings:\n");
    printf("  Mod+Space (press):  spawn 'echo start > /tmp/voiceinput-trigger'\n");
    printf("  Mod+Space (release): spawn 'echo stop > /tmp/voiceinput-trigger'\n");
    printf("\n");
    printf("Or use toggle mode:\n");
    printf("  echo trigger > /tmp/voiceinput-trigger\n");
    printf("\n");
    printf("Recording: NO TIME LIMIT - records until you send 'stop'\n");
    printf("\n");

    // Show startup notification
    show_notification("VoiceInput: Ready", "gtk-ok", 2000);

    while (g_running) {
        process_commands();
        usleep(10000);  // 10ms polling
    }

    if (g_recording) {
        stop_recording();
    }

    printf("\nShutting down...\n");
    cleanup_all();
    curl_global_cleanup();
    return 0;
}
