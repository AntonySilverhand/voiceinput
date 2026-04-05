#!/bin/bash
# Start voiceinput daemon manually (optional)
# The trigger.sh script will auto-start if needed

VOICEINPUT_BIN="/home/antony/coding/voiceinput/build/voiceinput"

# Check if already running
if pgrep -x "voiceinput" > /dev/null; then
    echo "voiceinput is already running (PID: $(pgrep -x voiceinput))"
    exit 0
fi

# Start daemon
echo "Starting voiceinput daemon..."
"$VOICEINPUT_BIN"
