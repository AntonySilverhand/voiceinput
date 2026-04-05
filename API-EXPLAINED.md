# Gemini API - Exactly What's Sent and Received

## One API Call Flow

```
YOUR PC (daemon.c)                              Google Gemini API
     │
     │  HTTP POST
     │  https://generativelanguage.googleapis.com/v1beta/models/
     │    gemini-2.5-flash-lite:generateContent?key=AIzaSy...
     │
     │  REQUEST BODY (JSON):
     ▼  ┌────────────────────────────────────────────────────────┐
        │ {                                                      │
        │   "contents": [                                        │
        │     {                                                  │
        │       "parts": [                                       │
        │         {                                              │
        │           "inline_data": {                             │
        │             "mime_type": "audio/wav",                  │
        │             "data": "<BASE64_ENCODED_WAV_FILE>"        │
        │           }                                            │
        │         }                                              │
        │       ]                                                │
        │     }                                                  │
        │   ],                                                   │
        │   "system_instruction": {                              │
        │     "parts": [{                                        │
        │       "text": "Transcribe the audio to text exactly.  │
        │                Return only the transcribed text,       │
        │                no explanations or commentary."         │
        │     }]                                                 │
        │   }                                                    │
        │ }                                                      │
        └────────────────────────────────────────────────────────┘
                                                              │
                                                              │
                                                              ▼
                                              ┌──────────────────────────┐
                                              │  Gemini 2.5 Flash Lite   │
                                              │  (Google's multimodal    │
                                              │   AI model)              │
                                              │                          │
                                              │  1. Decodes base64 → WAV │
                                              │  2. Runs audio through   │
                                              │     speech recognition   │
                                              │  3. Uses language model  │
                                              │     to refine text       │
                                              │  4. Returns transcription│
                                              └──────────┬───────────────┘
                                                         │
                                                         │  HTTP 200 OK
                                                         │  RESPONSE BODY:
                                                         ▼
                                              ┌──────────────────────────┐
                                              │ {                        │
                                              │   "candidates": [        │
                                              │     {                    │
                                              │       "content": {       │
                                              │         "parts": [       │
                                              │           {              │
                                              │             "text":      │
                                              │               "Hello     │
                                              │                world.    │
                                              │                This is   │
                                              │                what I    │
                                              │                said."    │
                                              │           }              │
                                              │         ]                │
                                              │       }                  │
                                              │     }                    │
                                              │   ]                      │
                                              │ }                        │
                                              └──────────────────────────┘
```

## What Gemini Does Internally

1. **Decodes base64** → Reconstructs WAV file
2. **Audio preprocessing** - Noise reduction, normalization
3. **Acoustic model** - Converts audio waveform to phonemes
4. **Language model** - Converts phonemes to words/text
5. **Context understanding** - Uses training to handle:
   - Accents
   - Homophones (their/there/they're)
   - Punctuation inference
   - Capitalization
6. **Returns plain text** - Just the transcription

## Your Audio → Their Text

**Example:**

**Sent (your microphone):**
```
[5 seconds of WAV audio, 16kHz, mono]
You speaking: "um hello world i think um coding is awesome"
```

**Received (from Gemini):**
```json
{
  "candidates": [{
    "content": {
      "parts": [{
        "text": "Hello world. I think coding is awesome."
      }]
    }
  }]
```

## File Size Example

**5 seconds of audio:**
- Raw float32 buffer: ~320 KB
- WAV (int16): ~160 KB  
- Base64 encoded: ~213 KB
- JSON payload: ~215 KB

**Response:**
- JSON with text: ~200 bytes

## API Endpoint Details

| Property | Value |
|----------|-------|
| **URL** | `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-lite:generateContent` |
| **Method** | POST |
| **Auth** | API key in URL (`?key=AIzaSy...`) |
| **Content-Type** | `application/json` |
| **Model** | `gemini-2.5-flash-lite` |
| **Input** | Base64-encoded WAV (audio/wav) |
| **Output** | JSON with text field |

## Why Base64?

JSON can't contain binary data directly. Base64 encoding:
- Converts binary → ASCII string
- Safe for JSON transmission
- ~33% size overhead (3 bytes → 4 characters)

## The Code (simplified)

```c
// 1. Your audio (float samples from mic)
float *audio = [...]  // 80,000 samples (5 sec @ 16kHz)

// 2. Convert to WAV file
char *wav = make_wav(audio, &wav_size);  // 160,044 bytes

// 3. Base64 encode
char *b64 = base64_encode(wav, wav_size);  // ~213,000 chars

// 4. Build JSON
json = {
  "contents": [{"parts": [{"inline_data": {
    "mime_type": "audio/wav",
    "data": b64
  }}]}],
  "system_instruction": {"parts": [{"text": "Transcribe..."}]}
}

// 5. HTTP POST
response = curl_post(url, json);

// 6. Parse response
text = response.candidates[0].content.parts[0].text;
```

## That's It - One Call

**Input:** Audio file (base64 in JSON)
**Output:** Text transcription

Gemini handles all the AI/ML internally - you just send audio, get text back.
