#include "voiceinput.h"
#include <libei-1.0/libei.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <linux/input.h>

// ============================================================================
// libei-based text injection
// Compositor handles stuck-key cleanup automatically if we crash.
// ============================================================================

// Evdev key codes are laid out by physical position, NOT alphabetically.
// A lookup table is required — arithmetic like KEY_A + (c - 'a') is wrong.
static const int letter_keycodes[26] = {
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
    KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
};

// Map ASCII characters to evdev key codes (US QWERTY layout)
static int ascii_to_keycode(int c) {
    if (c >= 'a' && c <= 'z') return letter_keycodes[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letter_keycodes[c - 'A'];
    if (c >= '1' && c <= '9') return KEY_1 + (c - '1');
    if (c == '0') return KEY_0;
    if (c == ' ') return KEY_SPACE;
    if (c == '.') return KEY_DOT;
    if (c == ',') return KEY_COMMA;
    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == '\t') return KEY_TAB;
    if (c == '-') return KEY_MINUS;
    if (c == '=') return KEY_EQUAL;
    if (c == '[') return KEY_LEFTBRACE;
    if (c == ']') return KEY_RIGHTBRACE;
    if (c == '\\') return KEY_BACKSLASH;
    if (c == ';') return KEY_SEMICOLON;
    if (c == '\'') return KEY_APOSTROPHE;
    if (c == '/') return KEY_SLASH;
    if (c == '`') return KEY_GRAVE;
    if (c == '!') return KEY_1;
    if (c == '@') return KEY_2;
    if (c == '#') return KEY_3;
    if (c == '$') return KEY_4;
    if (c == '%') return KEY_5;
    if (c == '^') return KEY_6;
    if (c == '&') return KEY_7;
    if (c == '*') return KEY_8;
    if (c == '(') return KEY_9;
    if (c == ')') return KEY_0;
    if (c == '_') return KEY_MINUS;
    if (c == '+') return KEY_EQUAL;
    if (c == '{') return KEY_LEFTBRACE;
    if (c == '}') return KEY_RIGHTBRACE;
    if (c == '|') return KEY_BACKSLASH;
    if (c == ':') return KEY_SEMICOLON;
    if (c == '"') return KEY_APOSTROPHE;
    if (c == '<') return KEY_COMMA;
    if (c == '>') return KEY_DOT;
    if (c == '?') return KEY_SLASH;
    if (c == '~') return KEY_GRAVE;
    return -1; // unsupported character
}

// Whether a character requires shift to type
static int ascii_needs_shift(int c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c == '!' || c == '@' || c == '#' || c == '$' ||
        c == '%' || c == '^' || c == '&' || c == '*' ||
        c == '(' || c == ')' || c == '_' || c == '+' ||
        c == '{' || c == '}' || c == '|' || c == ':' ||
        c == '"' || c == '<' || c == '>' || c == '?' ||
        c == '~') return 1;
    return 0;
}

struct libei_inject {
    struct ei *ctx;
    struct ei_seat *seat;
    struct ei_device *keyboard;
    int connected;
};

static int wait_for_event(struct ei *ctx, enum ei_event_type expected, int timeout_ms) {
    int fd = ei_get_fd(ctx);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;

    ei_dispatch(ctx);
    struct ei_event *event;
    while ((event = ei_get_event(ctx)) != NULL) {
        enum ei_event_type type = ei_event_get_type(event);
        ei_event_unref(event);
        if (type == expected) return 0;
        if (type == EI_EVENT_DISCONNECT) return -1;
    }
    return -2;
}

static int wait_for_keyboard_device(struct ei *ctx, int timeout_ms) {
    int fd = ei_get_fd(ctx);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;

    ei_dispatch(ctx);
    struct ei_event *event;
    while ((event = ei_get_event(ctx)) != NULL) {
        if (ei_event_get_type(event) == EI_EVENT_DEVICE_ADDED) {
            struct ei_device *dev = ei_event_get_device(event);
            if (dev && ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD)) {
                ei_event_unref(event);
                return 0;
            }
        }
        ei_event_unref(event);
    }
    return -1;
}

// Request a new keyboard device from the compositor via the existing seat.
static int libei_acquire_keyboard(struct libei_inject *li) {
    if (!li || !li->ctx || !li->seat) return -1;

    ei_seat_bind_capabilities(li->seat, EI_DEVICE_CAP_KEYBOARD, NULL);

    int found = 0;
    for (int i = 0; i < 50; i++) {
        if (wait_for_keyboard_device(li->ctx, 200) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) return -1;

    struct ei_event *event;
    ei_dispatch(li->ctx);
    while ((event = ei_get_event(li->ctx)) != NULL) {
        if (ei_event_get_type(event) == EI_EVENT_DEVICE_ADDED) {
            struct ei_device *dev = ei_event_get_device(event);
            if (dev && ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD)) {
                li->keyboard = dev;
                ei_device_ref(li->keyboard);
            }
        }
        ei_event_unref(event);
    }

    return li->keyboard ? 0 : -1;
}

// Release the keyboard device so the compositor drops it from input routing.
static void libei_release_keyboard(struct libei_inject *li) {
    if (!li || !li->keyboard) return;
    ei_device_stop_emulating(li->keyboard);
    ei_device_close(li->keyboard);
    ei_device_unref(li->keyboard);
    li->keyboard = NULL;
}

int vi_inject_libei_init(struct libei_inject *li) {
    if (!li) return -1;
    memset(li, 0, sizeof(*li));

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg) {
        fprintf(stderr, "libei: XDG_RUNTIME_DIR not set\n");
        return -1;
    }

    // Try to find an existing wayland socket
    char socket_path[512];
    const char *names[] = { "wayland-0", "wayland-1", NULL };
    int found_socket = -1;

    for (int i = 0; names[i]; i++) {
        snprintf(socket_path, sizeof(socket_path), "%s/%s", xdg, names[i]);
        struct stat st;
        if (stat(socket_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            found_socket = i;
            fprintf(stderr, "libei: found socket %s\n", socket_path);
            break;
        }
    }

    if (found_socket < 0) {
        fprintf(stderr, "libei: no wayland socket found in %s\n", xdg);
        return -1;
    }

    // Create sender context
    li->ctx = ei_new_sender(NULL);
    if (!li->ctx) {
        fprintf(stderr, "libei: failed to create sender context\n");
        return -1;
    }

    // Connect to the socket we found
    int sock_fd = ei_setup_backend_socket(li->ctx, socket_path);
    if (sock_fd < 0) {
        fprintf(stderr, "libei: failed to connect to %s\n", socket_path);
        ei_unref(li->ctx);
        li->ctx = NULL;
        return -1;
    }
    fprintf(stderr, "libei: connected (fd=%d)\n", sock_fd);

    // Wait for CONNECT event
    if (wait_for_event(li->ctx, EI_EVENT_CONNECT, 5000) != 0) {
        fprintf(stderr, "libei: connection to compositor failed or timed out\n");
        ei_unref(li->ctx);
        li->ctx = NULL;
        return -1;
    }
    fprintf(stderr, "libei: CONNECT received\n");

    // Wait for a seat to appear
    if (wait_for_event(li->ctx, EI_EVENT_SEAT_ADDED, 5000) != 0) {
        fprintf(stderr, "libei: no seat appeared from compositor\n");
        ei_disconnect(li->ctx);
        ei_unref(li->ctx);
        li->ctx = NULL;
        return -1;
    }
    fprintf(stderr, "libei: SEAT_ADDED received\n");

    // Get the seat from events and keep it for later device requests
    struct ei_event *event;
    ei_dispatch(li->ctx);
    while ((event = ei_get_event(li->ctx)) != NULL) {
        if (ei_event_get_type(event) == EI_EVENT_SEAT_ADDED) {
            struct ei_seat *s = ei_event_get_seat(event);
            if (s) {
                li->seat = s;
                ei_seat_ref(li->seat);
            }
        }
        ei_event_unref(event);
    }

    if (!li->seat) {
        fprintf(stderr, "libei: could not get seat\n");
        ei_disconnect(li->ctx);
        ei_unref(li->ctx);
        li->ctx = NULL;
        return -1;
    }

    // Acquire initial keyboard device to verify everything works
    if (libei_acquire_keyboard(li) < 0) {
        fprintf(stderr, "libei: no keyboard device appeared\n");
        ei_seat_unref(li->seat);
        li->seat = NULL;
        ei_disconnect(li->ctx);
        ei_unref(li->ctx);
        li->ctx = NULL;
        return -1;
    }

    li->connected = 1;
    fprintf(stderr, "libei: keyboard injection ready\n");
    return 0;
}

int vi_inject_libei_text(struct libei_inject *li, const char *text) {
    if (!li || !li->connected || !text) return -1;

    // Acquire a fresh keyboard device for this injection.
    // It will be released afterwards so the compositor immediately
    // drops it from input routing, giving the physical keyboard back.
    if (!li->keyboard && libei_acquire_keyboard(li) < 0) {
        fprintf(stderr, "libei: failed to acquire keyboard for injection\n");
        return -1;
    }

    ei_device_start_emulating(li->keyboard, 0);

    for (const char *p = text; *p; p++) {
        int c = (unsigned char)*p;
        int keycode = ascii_to_keycode(c);
        if (keycode < 0) {
            // Skip unsupported characters silently
            continue;
        }

        int needs_shift = ascii_needs_shift(c);
        uint64_t now = ei_now(li->ctx);

        if (needs_shift) {
            ei_device_keyboard_key(li->keyboard, KEY_LEFTSHIFT, true);
            ei_device_frame(li->keyboard, now);
        }

        now = ei_now(li->ctx);
        ei_device_keyboard_key(li->keyboard, keycode, true);
        ei_device_frame(li->keyboard, now);

        now = ei_now(li->ctx);
        ei_device_keyboard_key(li->keyboard, keycode, false);
        ei_device_frame(li->keyboard, now);

        if (needs_shift) {
            now = ei_now(li->ctx);
            ei_device_keyboard_key(li->keyboard, KEY_LEFTSHIFT, false);
            ei_device_frame(li->keyboard, now);
        }

        usleep(3000);
    }

    // Release the keyboard device — compositor removes the emulated
    // keyboard from its routing table so the physical keyboard works.
    libei_release_keyboard(li);
    return 0;
}

void vi_inject_libei_cleanup(struct libei_inject *li) {
    if (!li) return;
    libei_release_keyboard(li);
    if (li->seat) {
        ei_seat_unref(li->seat);
        li->seat = NULL;
    }
    if (li->ctx) {
        ei_disconnect(li->ctx);
        ei_unref(li->ctx);
        li->ctx = NULL;
    }
    li->connected = 0;
}

// ============================================================================
// Clipboard injection: wl-copy + wtype paste keystroke
// Detects terminal apps and uses Ctrl+Shift+V, otherwise Ctrl+V.
// ============================================================================

// Known terminal app_ids (checked against niri's focused-window app_id)
static int is_terminal_app(const char *app_id) {
    if (!app_id) return 0;
    const char *terminals[] = {
        "kitty", "alacritty", "foot", "wezterm", "ghostty",
        "gnome-terminal", "konsole", "xfce4-terminal", "tilix",
        "terminator", "sakura", "st", "urxvt", "xterm",
        NULL
    };
    for (int i = 0; terminals[i]; i++) {
        if (strstr(app_id, terminals[i]) != NULL) return 1;
    }
    return 0;
}

// Query niri for the focused window's app_id and window id.
// Returns window id (>0) on success, fills app_id buffer. Returns -1 on failure.
static int get_focused_window(char *app_id, size_t app_id_sz, int *window_id) {
    if (app_id) app_id[0] = '\0';
    if (window_id) *window_id = -1;

    FILE *fp = popen("niri msg -j focused-window 2>/dev/null", "r");
    if (!fp) return -1;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    if (n == 0) return -1;
    buf[n] = '\0';

    // Parse "app_id":"<value>"
    if (app_id) {
        const char *p = strstr(buf, "\"app_id\":\"");
        if (p) {
            p += strlen("\"app_id\":\"");
            const char *end = strchr(p, '"');
            if (end) {
                size_t len = (size_t)(end - p);
                if (len >= app_id_sz) len = app_id_sz - 1;
                memcpy(app_id, p, len);
                app_id[len] = '\0';
            }
        }
    }

    // Parse "id":<number>
    if (window_id) {
        const char *p = strstr(buf, "\"id\":");
        if (p) {
            *window_id = atoi(p + 5);
        }
    }

    return (window_id && *window_id > 0) ? *window_id : -1;
}

static int inject_via_clipboard(const char *text) {
    if (!text || strlen(text) == 0) return -1;

    char app_id[128];
    int win_id = -1;
    get_focused_window(app_id, sizeof(app_id), &win_id);
    int use_shift = is_terminal_app(app_id);

    fprintf(stderr, "Focused app: %s (paste: Ctrl+%sV, win_id=%d)\n",
            app_id[0] ? app_id : "unknown", use_shift ? "Shift+" : "", win_id);

    // Step 1: wl-copy — pipe text to it
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        execlp("wl-copy", "wl-copy", NULL);
        _exit(127);
    }

    close(pipefd[0]);
    write(pipefd[1], text, strlen(text));
    close(pipefd[1]);
    waitpid(pid, NULL, 0);

    // Brief pause for compositor to register clipboard
    usleep(30000);

    // Step 2: wtype paste
    pid = fork();
    if (pid == 0) {
        if (use_shift) {
            execlp("wtype", "wtype",
                   "-M", "ctrl", "-M", "shift", "-k", "v",
                   "-m", "shift", "-m", "ctrl", NULL);
        } else {
            execlp("wtype", "wtype",
                   "-M", "ctrl", "-k", "v", "-m", "ctrl", NULL);
        }
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    // Step 3: wtype's virtual keyboard corrupts niri's keyboard routing.
    // Fix it with a minimal focus cycle — switch to previous window and back.
    // Runs in background so it doesn't delay the return.
    if (win_id > 0) {
        if (fork() == 0) {
            usleep(10000);
            system("niri msg action focus-window-previous 2>/dev/null");
            usleep(10000);
            char cmd[64];
            snprintf(cmd, sizeof(cmd),
                     "niri msg action focus-window --id %d 2>/dev/null", win_id);
            system(cmd);
            _exit(0);
        }
    }

    return 0;
}

// ============================================================================
// wtype direct fallback (character-by-character keyboard simulation)
// ============================================================================

static int inject_via_wtype(const char *text) {
    if (!text || strlen(text) == 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        execlp("wtype", "wtype", "--", text, NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

// ============================================================================
// Public inject API — clipboard preferred, libei/wtype fallbacks
// ============================================================================

int vi_inject_init(vi_inject_ctx_t *ctx, vi_inject_method_t method) {
    (void)method;
    if (!ctx) return -1;

    memset(ctx, 0, sizeof(vi_inject_ctx_t));
    vi_desktop_t desktop = vi_desktop_detect();

    // Prefer libei on GNOME/Wayland or Niri as it's cleaner than clipboard
    if (desktop == VI_DESKTOP_GNOME || desktop == VI_DESKTOP_NIRI || vi_is_wayland()) {
        struct libei_inject *li = calloc(1, sizeof(struct libei_inject));
        if (li && vi_inject_libei_init(li) == 0) {
            libei_release_keyboard(li);
            ctx->libei_context = li;
            ctx->method = VI_INJECT_LIBEI;
            fprintf(stderr, "Text injector: using libei (detected desktop: %s)\n", vi_desktop_to_str(desktop));
            return 0;
        }
        free(li);
    }

    // Fall back to clipboard injection
    if (system("which wl-copy > /dev/null 2>&1") == 0 &&
        system("which wtype > /dev/null 2>&1") == 0) {
        ctx->method = VI_INJECT_CLIPBOARD;
        fprintf(stderr, "Text injector: using clipboard fallback (wl-copy + wtype)\n");
        return 0;
    }

    // Last resort: wtype direct
    if (system("which wtype > /dev/null 2>&1") == 0) {
        ctx->method = VI_INJECT_WTYPE;
        fprintf(stderr, "Text injector: using wtype (direct fallback)\n");
        return 0;
    }

    fprintf(stderr, "Text injector: no injection method available\n");
    return -1;
}

int vi_inject_text(vi_inject_ctx_t *ctx, const char *text) {
    if (!ctx || !text || strlen(text) == 0) return -1;

    if (ctx->method == VI_INJECT_CLIPBOARD) {
        int ret = inject_via_clipboard(text);
        if (ret == 0) return 0;
        fprintf(stderr, "Clipboard injection failed, trying fallbacks...\n");
    }

    if (ctx->libei_context) {
        int ret = vi_inject_libei_text(ctx->libei_context, text);
        if (ret == 0) return 0;
        fprintf(stderr, "libei injection failed, trying wtype...\n");
    }

    return inject_via_wtype(text);
}

void vi_inject_cleanup(vi_inject_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->libei_context) {
        vi_inject_libei_cleanup(ctx->libei_context);
        free(ctx->libei_context);
        ctx->libei_context = NULL;
    }
}
