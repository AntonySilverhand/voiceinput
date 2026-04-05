/**
 * VoiceInput FIFO Daemon
 * Listens on a FIFO pipe for trigger signals
 * When triggered: records audio, transcribes, injects text
 */

#include "voiceinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <curl/curl.h>

// ============================================================================
// Global State
// ============================================================================

static volatile sig_atomic_t g_running = 1;
static vi_audio_ctx_t g_audio;
static vi_gemini_ctx_t g_gemini;
static vi_config_t g_config;
static vi_inject_ctx_t g_injector;
static int g_fifo_fd = -1;

// ============================================================================
// Visual Feedback (using zenity)
// ============================================================================

static pid_t g_notification_pid = 0;

static void show_notification(const char *text, const char *icon, int timeout) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        char timeout_str[16];
        snprintf(timeout_str, sizeof(timeout_str), "%d", timeout);
        execlp("zenity", "zenity", "--notification",
               "--window-icon", icon, "--text", text,
               "--timeout", timeout_str, NULL);
        _exit(127);
    }
}

static void set_recording_state(int active) {
    if (active) {
        show_notification("VoiceInput: Recording... (speak now)", "gtk-media-record", 30);
    } else {
        if (g_notification_pid > 0) {
            kill(g_notification_pid, SIGTERM);
            g_notification_pid = 0;
        }
        system("pkill -f 'zenity.*Recording' 2>/dev/null || true");
    }
}

// ============================================================================
// Audio Processing
// ============================================================================

static int process_and_inject(void) {
    float *audio_data = NULL;
    size_t audio_len = 0;

    if (vi_audio_get_chunk(&g_audio, &audio_data, &audio_len) < 0 ||
        !audio_data || audio_len == 0) {
        fprintf(stderr, "No audio data\n");
        free(audio_data);
        return -1;
    }

    fprintf(stderr, "Processing %zu samples...\n", audio_len);

    char *transcribed = NULL;
    size_t transcribed_len = 0;

    // Transcribe with Gemini
    if (vi_gemini_transcribe(&g_gemini, audio_data, audio_len,
                             &transcribed, &transcribed_len) < 0) {
        fprintf(stderr, "Transcription failed\n");
        free(audio_data);
        show_notification("VoiceInput: Transcription failed", "gtk-dialog-error", 3);
        return -1;
    }

    free(audio_data);

    if (!transcribed || transcribed_len == 0) {
        fprintf(stderr, "Empty transcription\n");
        show_notification("VoiceInput: No speech detected", "gtk-dialog-warning", 2);
        return -1;
    }

    fprintf(stderr, "Transcribed: %s\n", transcribed);

    // Refine with Gemini
    char *refined = NULL;
    size_t refined_len = 0;

    if (g_config.refinement_enabled &&
        vi_gemini_refine(&g_gemini, transcribed, &refined, &refined_len) == 0 &&
        refined) {
        free(transcribed);
        transcribed = refined;
        transcribed_len = refined_len;
        fprintf(stderr, "Refined: %s\n", transcribed);
    }

    // Text processing
    if (g_config.remove_fillers) {
        vi_textproc_remove_fillers(transcribed);
    }
    vi_textproc_trim_whitespace(transcribed);

    // Inject text
    show_notification("VoiceInput: Typing...", "gtk-apply", 2);
    vi_inject_text(&g_injector, transcribed);

    free(transcribed);
    return 0;
}

// ============================================================================
// Recording
// ============================================================================

static int do_recording(int duration_ms) {
    fprintf(stderr, "Starting recording for %d ms...\n", duration_ms);

    set_recording_state(1);

    // Start audio capture
    if (vi_audio_start(&g_audio) < 0) {
        fprintf(stderr, "Failed to start audio\n");
        set_recording_state(0);
        return -1;
    }

    // Record for specified duration
    usleep(duration_ms * 1000);

    // Stop capture
    vi_audio_stop(&g_audio);
    set_recording_state(0);

    fprintf(stderr, "Recording stopped\n");

    // Process and inject
    show_notification("VoiceInput: Processing...", "gtk-dialog-info", 3);
    return process_and_inject();
}

// ============================================================================
// Signal Handlers
// ============================================================================

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// ============================================================================
// FIFO Setup
// ============================================================================

static int setup_fifo(void) {
    const char *fifo_path = "/tmp/voiceinput-trigger";

    // Remove existing FIFO
    unlink(fifo_path);

    // Create new FIFO
    if (mkfifo(fifo_path, 0666) < 0) {
        fprintf(stderr, "Failed to create FIFO: %s\n", strerror(errno));
        return -1;
    }

    // Open FIFO in non-blocking read mode
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

static int check_trigger(void) {
    if (g_fifo_fd < 0) return 0;

    char buf[64];
    ssize_t n = read(g_fifo_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, "trigger") != NULL || strstr(buf, "start") != NULL) {
            return 1;
        }
    } else if (n == 0) {
        // EOF — all writers closed. Reopen FIFO to accept new writers.
        close(g_fifo_fd);
        g_fifo_fd = open("/tmp/voiceinput-trigger", O_RDONLY | O_NONBLOCK);
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

    // Init Gemini
    if (vi_gemini_init(&g_gemini, api_key) < 0) {
        fprintf(stderr, "Failed to init Gemini\n");
        return -1;
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
    cleanup_fifo();
    vi_inject_cleanup(&g_injector);
    vi_audio_cleanup(&g_audio);
    vi_gemini_cleanup(&g_gemini);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("VoiceInput FIFO Daemon v%d.%d.%d\n",
           VI_VERSION_MAJOR, VI_VERSION_MINOR, VI_VERSION_PATCH);
    printf("Trigger: echo 'trigger' > /tmp/voiceinput-trigger\n\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (init_all() < 0) {
        fprintf(stderr, "Initialization failed\n");
        return 1;
    }

    printf("Daemon ready. Waiting for triggers...\n");
    printf("Niri binding: Mod+Space { spawn = [\"/home/antony/coding/voiceinput/trigger.sh\"]; }\n\n");

    while (g_running) {
        if (check_trigger()) {
            fprintf(stderr, "\n=== TRIGGER RECEIVED ===\n");
            do_recording(g_config.chunk_duration_ms);
            fprintf(stderr, "=== DONE ===\n\n");
        }
        usleep(50000);  // 50ms polling
    }

    printf("\nShutting down...\n");
    cleanup_all();
    curl_global_cleanup();
    return 0;
}
