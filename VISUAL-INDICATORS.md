# Visual Indicators - Where They Appear

## Current Setup (Zenity)

The visual indicators use **Zenity desktop notifications**, which appear as:

```
┌─────────────────────────────────────┐
│  🔴 VoiceInput: Recording...        │
│     (hold Super+Space)              │
└─────────────────────────────────────┘
```

**Position:** Top-center of screen (Zenity default)

**Behavior:**
- **Recording**: Persistent notification (stays while holding keys)
- **Processing**: 3-second notification
- **Error**: 3-second notification with error icon

## How It Works

1. **Press Super+Space** → Daemon starts (if not running)
2. **Hold keys** → Red "Recording..." notification appears top-center
3. **Speak** → Audio captured
4. **Release Space** → Recording stops, notification closes
5. **Processing** → Yellow "Processing..." notification (3 seconds)
6. **Text injected** → Green checkmark (brief)

## Testing the Indicator

```bash
# Test zenity notification directly
zenity --notification --window-icon=gtk-media-record \
       --text="VoiceInput: Recording..." &
```

## Alternative: Terminal Output

If you prefer terminal-based indicators (no GUI notifications), the daemon also outputs:

```
VoiceInput Daemon v0.1.0
Press-and-hold Super+Space to dictate

Loading configuration...
Initializing Gemini...
Initializing audio...
Initializing hotkey listener (Super+Space)...

Daemon running. Press-and-hold Super+Space to dictate.
Press Ctrl+C to exit.

Hotkey pressed - starting recording
Hotkey released - stopping recording
Transcription complete!
```

## Customizing Position/Style

Edit `src/daemon.c` function `set_visual_state()`:

- Change `--timeout` value for duration
- Change `--window-icon` for different icons:
  - `gtk-media-record` (red circle)
  - `gtk-dialog-info` (blue i)
  - `gtk-dialog-warning` (yellow triangle)
  - `gtk-dialog-error` (red X)

## Requirements

- **Zenity**: Already installed (`/usr/bin/zenity`)
- **Notification server**: Niri handles zenity notifications natively
