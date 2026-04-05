#!/bin/bash
# Start voiceinput daemon once - it handles Super+Space directly via libinput

DAEMON_BIN="/home/antony/coding/voiceinput/build/voiceinput-daemon"
PIDFILE="/tmp/voiceinput-daemon.pid"
LOGFILE="/tmp/voiceinput-daemon.log"

# Check if already running
if [ -f "$PIDFILE" ] && kill -0 $(cat "$PIDFILE") 2>/dev/null; then
    echo "✅ Daemon already running (PID: $(cat $PIDFILE))"
    exit 0
fi

# Start daemon
echo "Starting VoiceInput daemon..."
nohup "$DAEMON_BIN" > "$LOGFILE" 2>&1 &
echo $! > "$PIDFILE"

sleep 1

if kill -0 $(cat $PIDFILE) 2>/dev/null; then
    echo "✅ Daemon started (PID: $!)"
    echo ""
    echo "Usage:"
    echo "  1. Click into a text field"
    echo "  2. Press and HOLD Super+Space"
    echo "  3. Speak while holding"
    echo "  4. Release Space to stop and transcribe"
    echo ""
    echo "Logs: tail -f $LOGFILE"
else
    echo "❌ Failed to start daemon"
    exit 1
fi
