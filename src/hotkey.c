#include "voiceinput.h"
#include <libinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/input.h>

// ============================================================================
// Simple Global Hotkey using libinput
// Note: This is a simplified implementation. For production, consider using
// xkbcommon for proper key state tracking or a compositor-specific API.
// ============================================================================

static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface libinput_iface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

// Key code mappings
static int parse_key_code(const char *key_name) {
    struct {
        const char *name;
        int code;
    } key_map[] = {
        {"Space", KEY_SPACE},
        {"A", KEY_A}, {"B", KEY_B}, {"C", KEY_C}, {"D", KEY_D},
        {"E", KEY_E}, {"F", KEY_F}, {"G", KEY_G}, {"H", KEY_H},
        {"I", KEY_I}, {"J", KEY_J}, {"K", KEY_K}, {"L", KEY_L},
        {"M", KEY_M}, {"N", KEY_N}, {"O", KEY_O}, {"P", KEY_P},
        {"Q", KEY_Q}, {"R", KEY_R}, {"S", KEY_S}, {"T", KEY_T},
        {"U", KEY_U}, {"V", KEY_V}, {"W", KEY_W}, {"X", KEY_X},
        {"Y", KEY_Y}, {"Z", KEY_Z},
        {"0", KEY_0}, {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3},
        {"4", KEY_4}, {"5", KEY_5}, {"6", KEY_6}, {"7", KEY_7},
        {"8", KEY_8}, {"9", KEY_9},
        {"Return", KEY_ENTER}, {"Escape", KEY_ESC},
        {"BackSpace", KEY_BACKSPACE}, {"Tab", KEY_TAB},
        {NULL, -1}
    };

    for (int i = 0; key_map[i].name != NULL; i++) {
        if (strcmp(key_name, key_map[i].name) == 0) {
            return key_map[i].code;
        }
    }
    return -1;
}

static int parse_modifier_code(const char *modifier_name) {
    if (strcmp(modifier_name, "Ctrl") == 0 || strcmp(modifier_name, "Control") == 0) {
        return KEY_LEFTCTRL;
    } else if (strcmp(modifier_name, "Shift") == 0) {
        return KEY_LEFTSHIFT;
    } else if (strcmp(modifier_name, "Alt") == 0) {
        return KEY_LEFTALT;
    } else if (strcmp(modifier_name, "Super") == 0 || strcmp(modifier_name, "Win") == 0) {
        return KEY_LEFTMETA;
    }
    return -1;
}

int vi_hotkey_init(vi_hotkey_ctx_t *ctx, const char *modifier, const char *key) {
    if (!ctx || !modifier || !key) return -1;

    memset(ctx, 0, sizeof(vi_hotkey_ctx_t));
    strncpy(ctx->modifier, modifier, VI_MAX_HOTKEY_NAME - 1);
    strncpy(ctx->key, key, VI_MAX_HOTKEY_NAME - 1);

    // Parse key codes
    ctx->modifier_code = parse_modifier_code(modifier);
    ctx->key_code = parse_key_code(key);

    if (ctx->modifier_code < 0 || ctx->key_code < 0) {
        fprintf(stderr, "Invalid hotkey configuration: %s+%s\n", modifier, key);
        return -1;
    }

    // Create libinput context
    ctx->libinput_ctx = libinput_path_create_context(&libinput_iface, NULL);
    if (!ctx->libinput_ctx) {
        fprintf(stderr, "Failed to create libinput context\n");
        return -1;
    }

    // Try to open /dev/input/event0 (first keyboard device)
    // In production, you'd enumerate devices properly
    const char *device_paths[] = {
        "/dev/input/event0",
        "/dev/input/event1",
        "/dev/input/event2",
        "/dev/input/event3",
        NULL
    };

    for (int i = 0; device_paths[i] != NULL; i++) {
        ctx->libinput_device = libinput_path_add_device(ctx->libinput_ctx, device_paths[i]);
        if (ctx->libinput_device) {
            fprintf(stderr, "Opened input device: %s\n", device_paths[i]);
            break;
        }
    }

    if (!ctx->libinput_device) {
        fprintf(stderr, "Warning: Could not open any input device. Hotkey detection may not work.\n");
        fprintf(stderr, "Try running with appropriate permissions or check udev rules.\n");
        // Non-fatal - we'll still run but hotkeys won't work
    }

    ctx->modifier_pressed = 0;
    ctx->triggered = 0;

    return 0;
}

int vi_hotkey_check(vi_hotkey_ctx_t *ctx) {
    if (!ctx || !ctx->libinput_ctx) return 0;

    // Process pending events
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

            // Track modifier state
            if ((int)key == ctx->modifier_code) {
                ctx->modifier_pressed = (state == LIBINPUT_KEY_STATE_PRESSED);
            }

            // Check for hotkey combination
            if ((int)key == ctx->key_code &&
                state == LIBINPUT_KEY_STATE_PRESSED &&
                ctx->modifier_pressed) {
                ctx->triggered = 1;
            }
        }

        libinput_event_destroy(event);
    }

    int triggered = ctx->triggered;
    ctx->triggered = 0;  // Reset for next check

    return triggered;
}

void vi_hotkey_cleanup(vi_hotkey_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->libinput_device) {
        libinput_path_remove_device(ctx->libinput_device);
        ctx->libinput_device = NULL;
    }

    if (ctx->libinput_ctx) {
        libinput_unref(ctx->libinput_ctx);
        ctx->libinput_ctx = NULL;
    }

}
