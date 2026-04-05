#!/bin/bash
# Run voiceinput daemon with proper permissions
# This script can be called from Niri keybinding

DAEMON_BIN="/home/antony/coding/voiceinput/build/voiceinput-daemon"
PIDFILE="/tmp/voiceinput-daemon.pid"

# Check if already running
if [ -f "$PIDFILE" ] && kill -0 $(cat "$PIDFILE") 2>/dev/null; then
    echo "Daemon already running (PID: $(cat $PIDFILE))"
    exit 0
fi

# Start daemon in background
echo "Starting voiceinput daemon..."
nohup "$DAEMON_BIN" > /tmp/voiceinput-daemon.log 2>&1 &
echo $! > "$PIDFILE"

echo "Daemon started (PID: $!)"
echo "Press-and-hold Super+Space to dictate"
