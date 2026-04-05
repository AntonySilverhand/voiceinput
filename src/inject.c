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

// Map ASCII characters to evdev key codes (US QWERTY layout)
static int ascii_to_keycode(int c) {
    if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
    if (c >= 'A' && c <= 'Z') return KEY_A + (c - 'A');
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

    // Get the seat from events
    struct ei_event *event;
    struct ei_seat *seat = NULL;
    ei_dispatch(li->ctx);
    while ((event = ei_get_event(li->ctx)) != NULL) {
        if (ei_event_get_type(event) == EI_EVENT_SEAT_ADDED) {
            seat = ei_event_get_seat(event);
            if (seat) ei_seat_ref(seat);
        }
        ei_event_unref(event);
    }

    if (!seat) {
        fprintf(stderr, "libei: could not get seat\n");
        ei_disconnect(li->ctx);
        ei_unref(li->ctx);
        li->ctx = NULL;
        return -1;
    }

    // Bind keyboard capability
    ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, NULL);

    // Wait for keyboard device
    int found = 0;
    for (int i = 0; i < 50; i++) {
        if (wait_for_keyboard_device(li->ctx, 200) == 0) {
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "libei: no keyboard device appeared\n");
        ei_seat_unref(seat);
        ei_disconnect(li->ctx);
        ei_unref(li->ctx);
        li->ctx = NULL;
        return -1;
    }
    fprintf(stderr, "libei: keyboard device appeared\n");

    // Get the keyboard device
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

    ei_seat_unref(seat);

    if (!li->keyboard) {
        fprintf(stderr, "libei: could not obtain keyboard device\n");
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
    if (!li || !li->connected || !li->keyboard || !text) return -1;

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

    ei_device_stop_emulating(li->keyboard);
    return 0;
}

void vi_inject_libei_cleanup(struct libei_inject *li) {
    if (!li) return;
    if (li->keyboard) {
        ei_device_stop_emulating(li->keyboard);
        ei_device_close(li->keyboard);
        ei_device_unref(li->keyboard);
        li->keyboard = NULL;
    }
    if (li->ctx) {
        ei_disconnect(li->ctx);
        ei_unref(li->ctx);
        li->ctx = NULL;
    }
    li->connected = 0;
}

// ============================================================================
// wtype fallback (direct keyboard simulation)
// ============================================================================

static int inject_via_wtype(const char *text) {
    if (!text || strlen(text) == 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // Child: exec wtype directly, no shell involved
        execlp("wtype", "wtype", "--", text, NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

// ============================================================================
// Public inject API — auto-detect, libei first, wtype fallback
// ============================================================================

int vi_inject_init(vi_inject_ctx_t *ctx, vi_inject_method_t method) {
    (void)method; // libei is always preferred
    if (!ctx) return -1;

    memset(ctx, 0, sizeof(vi_inject_ctx_t));

    struct libei_inject *li = calloc(1, sizeof(struct libei_inject));
    if (!li) return -1;

    if (vi_inject_libei_init(li) == 0) {
        ctx->libei_context = li;
        fprintf(stderr, "Text injector: using libei (safe keyboard simulation)\n");
        return 0;
    }

    free(li);
    ctx->libei_context = NULL;

    // Fall back to wtype
    if (system("which wtype > /dev/null 2>&1") != 0) {
        fprintf(stderr, "Text injector: neither libei nor wtype available\n");
        return -1;
    }

    fprintf(stderr, "Text injector: using wtype (fallback)\n");
    return 0;
}

int vi_inject_text(vi_inject_ctx_t *ctx, const char *text) {
    if (!ctx || !text || strlen(text) == 0) return -1;

    if (ctx->libei_context) {
        int ret = vi_inject_libei_text(ctx->libei_context, text);
        if (ret == 0) return 0;
        fprintf(stderr, "libei injection failed, trying wtype fallback...\n");
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
