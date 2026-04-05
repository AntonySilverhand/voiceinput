#!/bin/bash
# Test Gemini API with actual audio file
# This bypasses recording and tests the core API flow

echo "=== Testing Gemini API with Audio ==="

# Check if test file exists, if not create silence
WAV_FILE="/tmp/test-voice.wav"
if [ ! -f "$WAV_FILE" ]; then
    echo "Creating 2-second silent WAV file..."
    # Generate silent WAV using ffmpeg
    ffmpeg -y -f lavfi -i "anullsrc=r=16000:cl=mono" -t 2 -ar 16000 -ac 1 "$WAV_FILE" 2>/dev/null
fi

if [ ! -f "$WAV_FILE" ]; then
    echo "❌ Cannot create audio file"
    exit 1
fi

echo "Audio file: $WAV_FILE ($(ls -lh $WAV_FILE | awk '{print $5}'))"

# Convert to base64
echo "Converting to base64..."
BASE64=$(base64 -w 0 "$WAV_FILE")
echo "Base64 length: ${#BASE64} chars"

# Send to Gemini
echo "Sending to Gemini API..."
RESPONSE=$(curl -s -X POST \
  "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-lite:generateContent?key=$GEMINI_API_KEY" \
  -H "Content-Type: application/json" \
  -d "{
    \"contents\": [{
      \"parts\": [{
        \"inline_data\": {
          \"mime_type\": \"audio/wav\",
          \"data\": \"$BASE64\"
        }
      }]
    }],
    \"system_instruction\": {
      \"parts\": [{
        \"text\": \"Transcribe the audio. If it's silence or no speech, say 'No speech detected'.\"
      }]
    }
  }")

echo ""
echo "Response:"
echo "$RESPONSE" | jq .

# Extract text
TEXT=$(echo "$RESPONSE" | jq -r '.candidates[0].content.parts[0].text' 2>/dev/null)
if [ -n "$TEXT" ] && [ "$TEXT" != "null" ]; then
    echo ""
    echo "✅ API returned text: $TEXT"
else
    echo ""
    echo "❌ API didn't return text"
    echo "Check if audio file is valid WAV"
    file "$WAV_FILE"
fi
