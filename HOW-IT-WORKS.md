# How VoiceInput Works - Complete Pipeline

## Current Architecture: Record-Then-Process (NOT Streaming)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         COMPLETE FLOW                                        │
└─────────────────────────────────────────────────────────────────────────────┘

  User Action          Daemon State           Data Flow
  ───────────          ────────────           ─────────

  1. Press Super+Space
                       │
                       ▼
                   ┌─────────────┐
                       IDLE
                       │
                       │ (libinput detects key press)
                       ▼
  2. Hold keys     ┌─────────────┐
                   │  RECORDING  │
                   │  🔴 Visual  │
                   │             │
                   │  PortAudio  │
                   │  captures   │
                   │  to ring    │
                   │  buffer     │
                   └──────┬──────┘
                          │
                          │ Audio accumulates in memory
                          │ (NOT sent anywhere yet)
                          │
  3. Release Space         │
                       ▼
                   ┌─────────────┐
                   │  PROCESSING │
                   │  🟡 Visual  │
                   │             │
                   │  Stop       │
                   │  recording  │
                   └──────┬──────┘
                          │
                          ▼
                   ┌─────────────────┐
                   │  Get all audio  │
                   │  from buffer    │
                   └────────┬────────┘
                            │
                            ▼
                   ┌─────────────────┐
                   │  Convert:       │
                   │  float32→int16  │
                   │  Add WAV header │
                   │  Base64 encode  │
                   └────────┬────────┘
                            │
                            ▼
                   ┌─────────────────┐
                   │  HTTP POST to   │
                   │  Gemini API     │
                   │  (whole file)   │
                   └────────┬────────┘
                            │
                            │ Wait for response...
                            │ (blocking)
                            ▼
                   ┌─────────────────┐
                   │  Parse JSON     │
                   │  Extract text   │
                   └────────┬────────┘
                            │
                            ▼
                   ┌─────────────────┐
                   │  Refine text    │
                   │  (Gemini API)   │
                   │  - Remove "um"  │
                   │  - Punctuation  │
                   └────────┬────────┘
                            │
                            ▼
                   ┌─────────────────┐
                   │  Inject via     │
                   │  wtype          │
                   │  (types text)   │
                   └────────┬────────┘
                            │
                            ▼
                       ┌─────────┐
                           IDLE
                       │  ✅ Done │
                       └─────────┘
```

## Key Characteristics

| Aspect | Current Implementation |
|--------|----------------------|
| **Recognition Mode** | Batch (record first, process after) |
| **Audio Storage** | In-memory ring buffer (~30 sec capacity) |
| **API Calls** | 2x HTTP (transcribe + refine) |
| **Latency** | Recording time + ~2-5s processing |
| **Streaming** | ❌ No real-time transcription |
| **Interrupt** | ❌ Can't see results while speaking |

## Why Not Streaming?

**Streaming would require:**
1. Chunked audio upload (WebSocket or chunked HTTP)
2. Server-side streaming response (SSE)
3. Incremental text injection (complex with window focus)
4. Handling partial results that change

**Current approach is simpler:**
- Single HTTP request
- Complete result before injection
- More reliable (no connection drops mid-sentence)
- Better for short dictations (< 30 seconds)

## Code Flow

### daemon.c - Main Loop
```c
while (g_running) {
    process_hotkey_event(&hotkey_ctx);  // Check Super+Space
    
    // On press:
    vi_audio_start(&g_audio);  // Start capturing to buffer
    set_visual_state(VISUAL_RECORDING);
    
    // On release:
    vi_audio_stop(&g_audio);   // Stop capturing
    
    // Process everything at once:
    vi_gemini_transcribe(...);  // Send full buffer
    vi_gemini_refine(...);      // Clean up text
    vi_inject_text(...);        // Type result
}
```

### Audio Buffer
```
Ring Buffer (in memory)
┌────────────────────────────────────────┐
│  [====recorded audio====][   free   ]  │
│   ↑                              ↑     │
│   read pos                    write pos│
└────────────────────────────────────────┘
Capacity: ~30 seconds at 16kHz mono
```

## Timing Example

```
t=0s:    Press Super+Space
         │
t=0.1s:  Recording starts (🔴 notification)
         │
t=5s:    Still holding, still recording
         │
t=8s:    Release Space
         │
t=8.1s:  Recording stops
         │
t=8.2s:  Processing starts (🟡 notification)
         │
t=10s:   Gemini API responds (~2s)
         │
t=10.5s: Text injected via wtype
         │
t=11s:   Back to idle (✅)
```

**Total time:** ~11 seconds for 8-second dictation

## Files Involved

```
src/daemon.c          Main event loop, hotkey handling
src/audio.c           PortAudio capture → ring buffer
src/gemini.c          HTTP POST to Gemini API
src/inject.c          wtype text injection
src/textproc.c        Filler word removal
src/config.c          Config loading + env detection
```

## To Add Streaming (Future Work)

Would need:
1. **WebSocket to Gemini** (or chunked upload)
2. **Incremental text buffer** (accumulate partial results)
3. **Smart injection** (don't type until confident)
4. **Revision handling** (Gemini may revise earlier text)

For now, batch processing is more reliable for the use case.
