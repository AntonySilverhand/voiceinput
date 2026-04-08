#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdbool.h>

typedef enum {
    VI_DESKTOP_UNKNOWN,
    VI_DESKTOP_GNOME,
    VI_DESKTOP_NIRI,
    VI_DESKTOP_SWAY,
    VI_DESKTOP_KDE,
    VI_DESKTOP_WAYLAND_GENERIC,
    VI_DESKTOP_X11
} vi_desktop_t;

vi_desktop_t vi_desktop_detect(void);
const char* vi_desktop_to_str(vi_desktop_t desktop);
bool vi_is_wayland(void);

#endif // DESKTOP_H
