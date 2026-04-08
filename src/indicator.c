#include "voiceinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

void vi_indicator_show_notification(const char *text, const char *icon, int timeout_ms) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char timeout_str[16];
        snprintf(timeout_str, sizeof(timeout_str), "%d", timeout_ms / 1000);

        if (timeout_ms > 0) {
            execlp("zenity", "zenity", "--notification",
                   "--window-icon", icon, "--text", text,
                   "--timeout", timeout_str, NULL);
        } else {
            execlp("zenity", "zenity", "--notification",
                   "--window-icon", icon, "--text", text, NULL);
        }
        _exit(127);
    }
}

static void close_all_notifications(void) {
    system("pkill -f 'zenity.*notification.*VoiceInput' 2>/dev/null || true");
}

void vi_indicator_set_state(vi_state_t state) {
    close_all_notifications();

    switch (state) {
        case VI_STATE_RECORDING:
            vi_indicator_show_notification("VoiceInput: Recording...", "gtk-media-record", 0);
            break;
        case VI_STATE_PROCESSING:
            vi_indicator_show_notification("VoiceInput: Processing...", "gtk-dialog-info", 3000);
            break;
        case VI_STATE_INJECTING:
            vi_indicator_show_notification("VoiceInput: Injecting...", "gtk-apply", 2000);
            break;
        case VI_STATE_IDLE:
        default:
            break;
    }
}
