#!/bin/bash
# Visual indicator overlay for VoiceInput
# Shows a floating status indicator on screen

STATUS="${1:-idle}"  # idle, recording, processing, error

# Colors
case "$STATUS" in
    recording)
        COLOR="#ef4444"  # Red
        TEXT="🔴 RECORDING"
        ;;
    processing)
        COLOR="#eab308"  # Yellow
        TEXT="🟡 PROCESSING"
        ;;
    error)
        COLOR="#dc2626"  # Dark red
        TEXT="❌ ERROR"
        ;;
    *)
        COLOR="#22c55e"  # Green
        TEXT="✓ VoiceInput Ready"
        ;;
esac

# Use yad for simple overlay (if available)
if command -v yad &> /dev/null; then
    case "$STATUS" in
        recording)
            # Persistent notification for recording
            yad --notification --image=gtk-media-record \
                --text="VoiceInput: Recording..." \
                --listen &
            echo $! > /tmp/voiceinput-indicator.pid
            ;;
        idle)
            # Kill any existing indicator
            if [ -f /tmp/voiceinput-indicator.pid ]; then
                kill $(cat /tmp/voiceinput-indicator.pid) 2>/dev/null
                rm -f /tmp/voiceinput-indicator.pid
            fi
            ;;
        *)
            # Brief notification
            yad --notification --image=gtk-dialog-info \
                --text="VoiceInput: $STATUS" \
                --timeout=2
            ;;
    esac
    exit 0
fi

# Fallback: Use Zenity if available
if command -v zenity &> /dev/null; then
    case "$STATUS" in
        recording)
            zenity --notification --text="VoiceInput: Recording..." &
            echo $! > /tmp/voiceinput-indicator.pid
            ;;
        idle)
            if [ -f /tmp/voiceinput-indicator.pid ]; then
                kill $(cat /tmp/voiceinput-indicator.pid) 2>/dev/null
                rm -f /tmp/voiceinput-indicator.pid
            fi
            ;;
        *)
            zenity --notification --text="VoiceInput: $STATUS" --timeout=2
            ;;
    esac
    exit 0
fi

# Ultimate fallback: Terminal bell + echo
echo -ne "\a"  # Terminal bell
echo "[$STATUS] VoiceInput: $TEXT"
