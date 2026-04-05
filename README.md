# VoiceInput

A fast, lightweight voice dictation tool for Linux (Wayland) built in C. Supports both Google Gemini and self-hosted Ollama models.

## Features

- **Gemini 2.5 Flash Lite** - Native audio transcription via Google Cloud API
- **Ollama** - Self-hosted whisper-tiny + gpt-oss:cloud
- **Global hotkey trigger** - Works with Niri compositor keybindings
- **FIFO-based triggering** - External scripts can trigger dictation
- **Text injection** - Uses `wtype` for Wayland-compatible text input
- **Transcription history** - SQLite-backed local storage

## Requirements

### Arch Linux Packages

```bash
sudo pacman -S portaudio curl cjson libinput libei sqlite cmake gcc make pkgconf wtype
```

## Building

```bash
cd /home/antony/coding/voiceinput
mkdir build && cd build
cmake ..
make
```

## Configuration

### 1. Set your Gemini API key

Edit `~/.config/voiceinput/config.json`:

```json
{
  "provider": "gemini",
  "gemini": {
    "api_key": "YOUR_GEMINI_API_KEY_HERE"
  }
}
```

Get an API key from: https://aistudio.google.com/apikey

### 2. Configure Niri keybinding

Add to your Niri config (`~/.config/niri/config.kdl`):

```kdl
keybinds = [
  {
    keys = ["Space"]
    action = { spawn = ["/home/antony/coding/voiceinput/trigger.sh"] }
  }
]
```

Then reload Niri config (usually `Super+Shift+R` or restart).

### 3. Full config options

```json
{
  "provider": "gemini",
  "ollama": {
    "host": "http://localhost:11434",
    "transcription_model": "whisper-tiny",
    "processing_model": "gpt-oss:cloud"
  },
  "gemini": {
    "api_key": "YOUR_API_KEY"
  },
  "hotkey": {
    "modifier": "Ctrl",
    "key": "Space"
  },
  "audio": {
    "sample_rate": 16000,
    "channels": 1,
    "chunk_duration_ms": 5000
  },
  "text_injection": {
    "method": "wtype",
    "fallback": "wtype"
  },
  "refinement": {
    "enabled": true,
    "remove_fillers": true,
    "auto_punctuate": true
  },
  "history": {
    "enabled": true,
    "max_entries": 1000
  }
}
```

## Usage

### Method 1: Niri Keybinding (Recommended)

1. Add the keybinding to Niri config (see above)
2. Run voiceinput in background:
   ```bash
   ./build/voiceinput &
   ```
3. Press **Space** to dictate - text appears at cursor!

### Method 2: Manual Trigger

```bash
# Start voiceinput
./build/voiceinput &

# Trigger from anywhere
echo "trigger" > /tmp/voiceinput-trigger
```

### Method 3: Hotkey (Fallback)

If libinput works in your setup:
- Press **Ctrl+Space** to start dictation
- Release to stop and transcribe

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Trigger (Niri keybind / FIFO / libinput hotkey)        │
├─────────────────────────────────────────────────────────┤
│  Audio Capture (PortAudio, 16kHz mono)                  │
├─────────────────────────────────────────────────────────┤
│  Transcription Provider                                 │
│    - Gemini 2.5 Flash Lite (native audio)               │
│    - Ollama (whisper-tiny)                              │
├─────────────────────────────────────────────────────────┤
│  AI Refinement (Gemini or gpt-oss:cloud)                │
├─────────────────────────────────────────────────────────┤
│  Text Processor (filler removal, punctuation)           │
├─────────────────────────────────────────────────────────┤
│  Text Injector (wtype for Wayland)                      │
├─────────────────────────────────────────────────────────┤
│  History Storage (SQLite)                               │
└─────────────────────────────────────────────────────────┘
```

## Troubleshooting

### Gemini API errors
- Check your API key is set in config
- Verify network connectivity: `curl https://generativelanguage.googleapis.com`

### Text not appearing
- Ensure `wtype` is installed: `which wtype`
- Test wtype manually: `echo "test" | wtype -`

### Trigger not working
- Check FIFO exists: `ls -la /tmp/voiceinput-trigger`
- Ensure voiceinput daemon is running

### Audio not captured
- Check microphone permissions
- Test with: `arecord -d 2 | aplay`

## Project Structure

```
voiceinput/
├── src/
│   ├── main.c           # Entry point, event loop, FIFO trigger
│   ├── audio.c          # PortAudio audio capture
│   ├── ollama.c         # Ollama API client
│   ├── gemini.c         # Gemini API client (native audio)
│   ├── hotkey.c         # libinput hotkey listener
│   ├── inject.c         # Text injection (wtype)
│   ├── textproc.c       # Text processing/cleanup
│   ├── config.c         # JSON configuration parser
│   └── history.c        # SQLite history storage
├── include/
│   └── voiceinput.h     # Common headers/types
├── trigger.sh           # Niri trigger script
├── niri-config.kdl      # Example Niri config snippet
├── CMakeLists.txt       # Build configuration
├── config.example.json  # Example configuration
└── README.md            # This file
```

## License

MIT License

## Acknowledgments

Inspired by:
- [Wispr Flow](https://wisprflow.ai/) - Voice dictation with AI editing
- [Voquill](https://voquill.com/) - Open-source voice typing
