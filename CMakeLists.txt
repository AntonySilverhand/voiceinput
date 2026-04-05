cmake_minimum_required(VERSION 3.16)
project(voiceinput VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find dependencies
find_package(PkgConfig REQUIRED)

pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(JSONC REQUIRED json-c)
pkg_check_modules(LIBINPUT REQUIRED libinput)
pkg_check_modules(LIBEI REQUIRED libei-1.0)
pkg_check_modules(SQLITE3 sqlite3)

# Source files for main voiceinput (FIFO trigger mode)
set(SOURCES
    src/main.c
    src/audio.c
    src/gemini.c
    src/hotkey.c
    src/inject.c
    src/textproc.c
    src/config.c
    src/history.c
)

# Source files for daemon (press-and-hold mode)
set(DAEMON_SOURCES
    src/daemon.c
    src/audio.c
    src/gemini.c
    src/hotkey.c
    src/inject.c
    src/textproc.c
    src/config.c
)

# Header include directory
include_directories(include ${PORTAUDIO_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS}
                    ${JSONC_INCLUDE_DIRS} ${LIBINPUT_INCLUDE_DIRS} ${LIBEI_INCLUDE_DIRS}
                    ${SQLITE3_INCLUDE_DIRS})

# Create main executable (FIFO trigger mode)
add_executable(voiceinput ${SOURCES})

# Create daemon executable (press-and-hold mode)
add_executable(voiceinput-daemon ${DAEMON_SOURCES})

# Link libraries for daemon
target_link_libraries(voiceinput-daemon
    ${PORTAUDIO_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONC_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${LIBEI_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    m
    pthread
)

# Compiler flags for daemon
target_compile_options(voiceinput-daemon PRIVATE
    -Wall -Wextra -Wpedantic
    ${PORTAUDIO_CFLAGS_OTHER}
    ${LIBCURL_CFLAGS_OTHER}
)

# Validate API key tool
add_executable(validate-gemini-key src/validate-key.c)
target_link_libraries(validate-gemini-key
    ${LIBCURL_LIBRARIES}
    ${JSONC_LIBRARIES}
)

# FIFO daemon (for Niri keybinding)
set(FIFO_SOURCES
    src/fifo-daemon.c
    src/audio.c
    src/gemini.c
    src/textproc.c
    src/inject.c
    src/config.c
)
add_executable(voiceinput-fifo ${FIFO_SOURCES})
target_link_libraries(voiceinput-fifo
    ${PORTAUDIO_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONC_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${LIBEI_LIBRARIES}
    m
    pthread
)

# Niri daemon (start/stop control, no time limit)
set(NIRI_SOURCES
    src/niri-daemon.c
    src/audio.c
    src/gemini.c
    src/textproc.c
    src/inject.c
    src/config.c
)
add_executable(voiceinput-niri ${NIRI_SOURCES})
target_link_libraries(voiceinput-niri
    ${PORTAUDIO_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONC_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${LIBEI_LIBRARIES}
    m
    pthread
)

# Link libraries
target_link_libraries(voiceinput
    ${PORTAUDIO_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONC_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${LIBEI_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    m
    pthread
)

# Compiler flags
target_compile_options(voiceinput PRIVATE
    -Wall -Wextra -Wpedantic
    ${PORTAUDIO_CFLAGS_OTHER}
    ${LIBCURL_CFLAGS_OTHER}
)

# Install
install(TARGETS voiceinput DESTINATION bin)
install(FILES config.example.json DESTINATION share/voiceinput)
