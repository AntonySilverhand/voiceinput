#!/bin/bash
# Install voiceinput to system

VOICEINPUT_DIR="/home/antony/coding/voiceinput"
BUILD_DIR="$VOICEINPUT_DIR/build"

echo "Building voiceinput..."
cd "$BUILD_DIR"
make

echo ""
echo "Creating symlinks in /usr/local/bin..."
sudo ln -sf "$BUILD_DIR/voiceinput" /usr/local/bin/voiceinput
sudo ln -sf "$BUILD_DIR/voiceinput-daemon" /usr/local/bin/voiceinput-daemon
sudo ln -sf "$VOICEINPUT_DIR/trigger.sh" /usr/local/bin/voiceinput-trigger

echo ""
echo "Installation complete!"
echo ""
echo "Usage options:"
echo "  1. FIFO mode (background daemon):"
echo "     voiceinput &"
echo "     Then use trigger.sh or echo 'trigger' > /tmp/voiceinput-trigger"
echo ""
echo "  2. Press-and-hold mode (direct Super+Space):"
echo "     sudo voiceinput-daemon"
echo "     Then press-and-hold Super+Space to dictate"
echo ""
