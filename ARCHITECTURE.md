# VoiceInput Architecture & Pipeline

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              USER INTERACTION                                │
│                                                                              │
│   ┌─────────────────┐         ┌──────────────────┐         ┌─────────────┐ │
│   │  Press Super    │         │   Hold & Speak   │         │   Release   │ │
│   │  + Space        │────────▶│   (Recording)    │────────▶│   Space     │ │
│   └─────────────────┘         └──────────────────┘         └──────┬──────┘ │
│                                                                    │        │
└────────────────────────────────────────────────────────────────────┼────────┘
                                                                     │
                                                                     ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           VOICEINPUT DAEMON                                  │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  1. HOTKEY LISTENER (libinput)                                      │   │
│   │     - Monitors /dev/input/event* for keyboard events                │   │
│   │     - Detects Super+Space press/release                             │   │
│   │     - Triggers recording state changes                              │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                        │
│                                    ▼                                        │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  2. AUDIO CAPTURE (PortAudio)                                       │   │
│   │     - Opens default microphone input                                │   │
│   │     - 16kHz, mono, float32 PCM                                      │   │
│   │     - Ring buffer for streaming                                     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                        │
│                                    ▼                                        │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  3. AUDIO PROCESSING                                                │   │
│   │     - Convert float32 → int16 PCM                                   │   │
│   │     - Add WAV header (44 bytes)                                     │   │
│   │     - Base64 encode for API                                         │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                        │
│                                    ▼                                        │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  4. GEMINI API (HTTP/libcurl)                                       │   │
│   │     - POST to gemini-2.5-flash-lite                                 │   │
│   │     - Native audio input (base64 WAV)                               │   │
│   │     - Returns transcribed text                                      │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                        │
│                                    ▼                                        │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  5. TEXT REFINEMENT (Gemini)                                        │   │
│   │     - Remove filler words (um, uh, like)                            │   │
│   │     - Add punctuation & capitalization                              │   │
│   │     - Preserve original meaning                                     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                        │
│                                    ▼                                        │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  6. TEXT INJECTION (wtype)                                          │   │
│   │     - Send refined text to active window                            │   │
│   │     - Wayland-compatible input                                      │   │
│   │     - Types at cursor position                                      │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                        │
│                                    ▼                                        │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  7. VISUAL FEEDBACK (notify-send)                                   │   │
│   │     - "Recording..." indicator                                      │   │
│   │     - "Processing..." status                                        │   │
│   │     - Error notifications                                           │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Data Flow

```
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  Microphone  │───▶│  PortAudio   │───▶│  Ring Buffer │
│  (Hardware)  │    │  (Capture)   │    │  (Memory)    │
└──────────────┘    └──────────────┘    └──────────────┘
                                              │
                                              ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│   Cursor     │◀───│    wtype     │◀───│   Refined    │
│   Position   │    │  (Inject)    │    │    Text      │
└──────────────┘    └──────────────┘    └──────────────┘
                                              ▲
                                              │
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  Gemini API  │◀───│  Base64 WAV  │◀───│  int16 PCM   │
│  (Cloud)     │    │  (Encoded)   │    │  (Formatted) │
└──────────────┘    └──────────────┘    └──────────────┘
```

## State Machine

```
                    ┌─────────────┐
                    │    IDLE     │
                    │ (Visual: -) │
                    └──────┬──────┘
                           │
                           │ Super+Space PRESS
                           ▼
                    ┌─────────────┐
          ┌────────│  RECORDING  │────────┐
          │        │(Visual: 🔴) │        │
          │        └──────┬──────┘        │
          │               │               │
          │               │ Audio         │
          │               │ captured      │
          │               │               │
          │               ▼               │
          │        ┌─────────────┐        │
          │        │  PROCESSING │        │ Error
          │        │(Visual: 🟡) │◀───────┘
          │        └──────┬──────┘
          │               │
          │               │ Success
          │               ▼
          │        ┌─────────────┐
          │        │  INJECTING  │
          │        │(Visual: 🟢) │
          │        └──────┬──────┘
          │               │
          │               │ Text typed
          │               ▼
          └──────────────┴───────▶ (back to IDLE)
```

## File Structure

```
/home/antony/coding/voiceinput/
├── src/
│   ├── main.c           # FIFO-trigger mode daemon
│   ├── daemon.c         # Press-and-hold mode daemon
│   ├── audio.c          # PortAudio capture
│   ├── gemini.c         # Gemini API client
│   ├── ollama.c         # Ollama API client (alternative)
│   ├── hotkey.c         # libinput hotkey listener
│   ├── inject.c         # wtype text injection
│   ├── textproc.c       # Text cleanup
│   ├── config.c         # JSON config + env detection
│   └── history.c        # SQLite history
├── include/
│   └── voiceinput.h     # Shared headers
├── build/
│   ├── voiceinput       # FIFO mode binary
│   └── voiceinput-daemon # Press-and-hold binary
├── trigger.sh           # Niri trigger script
├── install.sh           # Installation script
└── config.example.json  # Configuration template
```

## Configuration Flow

```
┌─────────────────────────────────────────────────────────────┐
│                    CONFIG LOADING                            │
│                                                              │
│  1. Load ~/.config/voiceinput/config.json                   │
│  2. Parse provider setting ("gemini" or "ollama")           │
│  3. Check GEMINI_API_KEY in config                          │
│  4. Override with /etc/environment GEMINI_API_KEY           │
│     (Environment takes precedence)                          │
└─────────────────────────────────────────────────────────────┘
```

## Visual Indicators

| State | Notification | Duration |
|-------|-------------|----------|
| Recording | "VoiceInput: Recording..." | Persistent (until release) |
| Processing | "VoiceInput: Processing..." | 2 seconds |
| Error | "VoiceInput: Error occurred" | 3 seconds |
| Idle | (none) | - |

## Audio Pipeline Details

```
Analog Sound
     │
     ▼
┌─────────────────┐
│ ALSA/PulseAudio │
│ (System Audio)  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   PortAudio     │
│ (Cross-platform │
│  audio I/O)     │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ float32 PCM     │
│ 16kHz, mono     │
│ Ring buffer     │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ int16 PCM       │
│ Normalized      │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ WAV header +    │
│ PCM data        │
│ (44 + N bytes)  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Base64 encoded  │
│ (Safe for JSON) │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Gemini API      │
│ gemini-2.5-     │
│ flash-lite      │
└─────────────────┘
```

## Dependencies

| Library | Purpose | Arch Package |
|---------|---------|--------------|
| PortAudio | Audio capture | `portaudio` |
| libcurl | HTTP client | `curl` |
| json-c | JSON parsing | `json-c` |
| libinput | Keyboard events | `libinput` |
| wtype | Text injection | `wtype` |
| notify-send | Visual feedback | `libnotify` |

## Running Modes

### Mode 1: Press-and-Hold (Recommended)
```bash
sudo ./build/voiceinput-daemon
# Then: Press-and-hold Super+Space
```

### Mode 2: FIFO Trigger
```bash
./build/voiceinput &
# Then: echo "trigger" > /tmp/voiceinput-trigger
```

## Niri Integration

The keybinding in `~/.config/niri/binds.kdl`:
```kdl
Mod+Space { spawn "systemctl --user restart voiceinput-daemon || /home/antony/coding/voiceinput/build/voiceinput-daemon &"; }
```

This ensures the daemon is running when you press the hotkey.
