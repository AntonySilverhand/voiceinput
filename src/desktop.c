#include "desktop.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

bool vi_is_wayland(void) {
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    return (wayland_display != NULL && strlen(wayland_display) > 0);
}

vi_desktop_t vi_desktop_detect(void) {
    const char *xdg_current = getenv("XDG_CURRENT_DESKTOP");

    if (xdg_current) {
        if (strstr(xdg_current, "GNOME")) return VI_DESKTOP_GNOME;
        if (strstr(xdg_current, "niri")) return VI_DESKTOP_NIRI;
        if (strstr(xdg_current, "KDE")) return VI_DESKTOP_KDE;
        if (strstr(xdg_current, "sway")) return VI_DESKTOP_SWAY;
    }

    // Fallback detection via other env vars
    const char *desktop_session = getenv("DESKTOP_SESSION");
    if (desktop_session) {
        if (strstr(desktop_session, "gnome")) return VI_DESKTOP_GNOME;
        if (strstr(desktop_session, "niri")) return VI_DESKTOP_NIRI;
    }

    if (vi_is_wayland()) return VI_DESKTOP_WAYLAND_GENERIC;

    const char *display = getenv("DISPLAY");
    if (display && strlen(display) > 0) return VI_DESKTOP_X11;

    return VI_DESKTOP_UNKNOWN;
}

const char* vi_desktop_to_str(vi_desktop_t desktop) {
    switch (desktop) {
        case VI_DESKTOP_GNOME: return "GNOME";
        case VI_DESKTOP_NIRI: return "Niri";
        case VI_DESKTOP_KDE: return "KDE";
        case VI_DESKTOP_SWAY: return "Sway";
        case VI_DESKTOP_WAYLAND_GENERIC: return "Wayland (Generic)";
        case VI_DESKTOP_X11: return "X11";
        default: return "Unknown";
    }
}
