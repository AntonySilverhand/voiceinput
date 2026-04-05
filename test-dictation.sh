#!/bin/bash
# Test voiceinput dictation without using hotkeys
# Records 3 seconds of audio and transcribes it

echo "=== VoiceInput Test ==="
echo ""

# Check API key
if [ -z "$GEMINI_API_KEY" ]; then
    echo "❌ GEMINI_API_KEY not set"
    exit 1
fi

echo "✅ API key found"

# Validate API key
echo "Validating API key..."
./build/validate-gemini-key
if [ $? -ne 0 ]; then
    echo "❌ API key validation failed"
    exit 1
fi

echo ""
echo "=== Testing Audio Recording ==="
echo "Speak for 3 seconds..."
echo ""

# Record 3 seconds of audio using parecord (PulseAudio)
echo "Recording 3 seconds... speak now!"
parecord --device=alsa_input.pci-0000_00_1f.3.analog-stereo --rate=16000 --channels=1 --format=s16le --time-limit=3 /tmp/test-voice.wav 2>/dev/null

# Fallback to ffmpeg if parecord fails
if [ ! -f /tmp/test-voice.wav ]; then
    echo "Trying ffmpeg..."
    ffmpeg -y -f pulse -i default -t 3 -ar 16000 -ac 1 /tmp/test-voice.wav 2>/dev/null
fi

if [ ! -f /tmp/test-voice.wav ]; then
    echo "❌ Failed to record audio"
    echo "   Check: pactl list sources short"
    exit 1
fi

echo "✅ Audio recorded: /tmp/test-voice.wav"
echo ""

# Convert to base64 and send to Gemini
echo "Sending to Gemini API..."
BASE64_AUDIO=$(base64 -w 0 /tmp/test-voice.wav)

RESPONSE=$(curl -s -X POST \
  "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-lite:generateContent?key=$GEMINI_API_KEY" \
  -H "Content-Type: application/json" \
  -d "{
    \"contents\": [{
      \"parts\": [{
        \"inline_data\": {
          \"mime_type\": \"audio/wav\",
          \"data\": \"$BASE64_AUDIO\"
        }
      }]
    }],
    \"system_instruction\": {
      \"parts\": [{
        \"text\": \"Transcribe the audio to text exactly. Return only the transcribed text.\"
      }]
    }
  }")

echo "Response:"
echo "$RESPONSE" | jq -r '.candidates[0].content.parts[0].text' 2>/dev/null || echo "$RESPONSE"

echo ""
echo "=== Testing Text Injection ==="
TEST_TEXT="Hello from voiceinput test!"
echo "Injecting: $TEST_TEXT"

# Give user time to focus a text field
sleep 2
echo "$TEST_TEXT" | wtype -

if [ $? -eq 0 ]; then
    echo "✅ Text injection successful"
else
    echo "❌ Text injection failed"
fi

echo ""
echo "=== Test Complete ==="

# Cleanup
rm -f /tmp/test-voice.wav
