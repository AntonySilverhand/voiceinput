#!/bin/bash
# Toggle script for Niri keybinding
# Signals the voiceinput daemon to toggle recording

FIFO="/tmp/voiceinput-trigger"
DAEMON_BIN="/home/antony/coding/voiceinput/build/voiceinput"

# Clear any stuck notifications
pkill -f 'zenity.*VoiceInput' 2>/dev/null || true

# Check if daemon is running (match either old or new binary name)
if ! pgrep -f "voiceinput$" > /dev/null; then
    echo "Starting voiceinput daemon..."
    nohup "$DAEMON_BIN" > /tmp/voiceinput.log 2>&1 &
    sleep 1
fi

if [ ! -p "$FIFO" ]; then
    echo "Error: FIFO not available"
    exit 1
fi

# Send toggle command
echo "trigger" > "$FIFO"
