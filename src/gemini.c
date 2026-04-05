#include "voiceinput.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Base64 Encoding for Gemini API
// ============================================================================

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode_raw(const uint8_t *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *encoded = malloc(out_len + 1);
    if (!encoded) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = i < len ? data[i] : 0;
        uint32_t octet_b = i + 1 < len ? data[i + 1] : 0;
        uint32_t octet_c = i + 2 < len ? data[i + 2] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded[j++] = base64_table[(triple >> 18) & 0x3F];
        encoded[j++] = base64_table[(triple >> 12) & 0x3F];
        encoded[j++] = base64_table[(triple >> 6) & 0x3F];
        encoded[j++] = base64_table[triple & 0x3F];
    }

    // Add padding
    size_t mod = len % 3;
    if (mod == 1) {
        encoded[out_len - 1] = '=';
        encoded[out_len - 2] = '=';
    } else if (mod == 2) {
        encoded[out_len - 1] = '=';
    }

    encoded[out_len] = '\0';
    return encoded;
}

// ============================================================================
// HTTP Response Buffer
// ============================================================================

struct gemini_response {
    char *data;
    size_t size;
};

static size_t gemini_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct gemini_response *mem = (struct gemini_response *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

// ============================================================================
// Gemini API Implementation
// ============================================================================

int vi_gemini_init(vi_gemini_ctx_t *ctx, const char *api_key) {
    if (!ctx || !api_key) return -1;

    memset(ctx, 0, sizeof(vi_gemini_ctx_t));
    strncpy(ctx->api_key, api_key, VI_MAX_API_KEY_LEN - 1);

    // Note: caller must call curl_global_init() once before this
    ctx->curl_handle = curl_easy_init();
    if (!ctx->curl_handle) {
        return -1;
    }

    return 0;
}

static char *normalize_audio_to_wav(const float *audio, size_t len, size_t *out_len) {
    // Convert float32 to int16 PCM (16kHz, mono)
    int16_t *pcm = malloc(len * sizeof(int16_t));
    if (!pcm) return NULL;

    for (size_t i = 0; i < len; i++) {
        float val = audio[i];
        if (val > 1.0f) val = 1.0f;
        if (val < -1.0f) val = -1.0f;
        pcm[i] = (int16_t)(val * 32767.0f);
    }

    // Simple WAV header (44 bytes)
    size_t wav_size = len * sizeof(int16_t) + 44;
    char *wav_data = malloc(wav_size);
    if (!wav_data) {
        free(pcm);
        return NULL;
    }

    // WAV header
    memcpy(wav_data, "RIFF", 4);
    uint32_t file_size = wav_size - 8;
    memcpy(wav_data + 4, &file_size, 4);
    memcpy(wav_data + 8, "WAVE", 4);
    memcpy(wav_data + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(wav_data + 16, &fmt_size, 4);
    uint16_t audio_format = 1;  // PCM
    memcpy(wav_data + 20, &audio_format, 2);
    uint16_t channels = 1;
    memcpy(wav_data + 22, &channels, 2);
    uint32_t sample_rate = 16000;
    memcpy(wav_data + 24, &sample_rate, 4);
    uint32_t byte_rate = sample_rate * channels * 2;
    memcpy(wav_data + 28, &byte_rate, 4);
    uint16_t block_align = channels * 2;
    memcpy(wav_data + 32, &block_align, 2);
    uint16_t bits_per_sample = 16;
    memcpy(wav_data + 34, &bits_per_sample, 2);
    memcpy(wav_data + 36, "data", 4);
    uint32_t data_size = len * sizeof(int16_t);
    memcpy(wav_data + 40, &data_size, 4);

    // Copy PCM data
    memcpy(wav_data + 44, pcm, len * sizeof(int16_t));

    *out_len = wav_size;
    free(pcm);

    return wav_data;
}

int vi_gemini_transcribe(vi_gemini_ctx_t *ctx, const float *audio,
                         size_t len, char **text, size_t *text_len) {
    if (!ctx || !audio || !text || !text_len) return -1;

    *text = NULL;
    *text_len = 0;

    // Normalize audio to WAV format
    size_t wav_len;
    char *wav_data = normalize_audio_to_wav(audio, len, &wav_len);
    if (!wav_data) return -1;

    // Base64 encode the WAV data
    char *base64_audio = base64_encode_raw((const uint8_t *)wav_data, wav_len);
    free(wav_data);

    if (!base64_audio) return -1;

    // Build Gemini API request
    // Gemini uses a different format: POST to generativeLanguage API
    char url[512];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/gemini-3-flash-preview:generateContent?key=%s",
             ctx->api_key);

    // Build JSON payload with inline_data for audio
    struct json_object *root = json_object_new_object();
    struct json_object *contents = json_object_new_array();
    struct json_object *content = json_object_new_object();
    struct json_object *parts = json_object_new_array();
    struct json_object *part = json_object_new_object();
    struct json_object *inline_data = json_object_new_object();

    json_object_object_add(inline_data, "mime_type",
                           json_object_new_string("audio/wav"));
    json_object_object_add(inline_data, "data",
                           json_object_new_string(base64_audio));
    json_object_object_add(part, "inline_data", inline_data);
    json_object_array_add(parts, part);
    json_object_object_add(content, "parts", parts);
    json_object_array_add(contents, content);
    json_object_object_add(root, "contents", contents);

    // Add system instruction for transcription
    struct json_object *system_instruction = json_object_new_object();
    struct json_object *si_parts = json_object_new_array();
    struct json_object *si_part = json_object_new_object();
    json_object_object_add(si_part, "text",
                           json_object_new_string("Transcribe the audio to text exactly. Return only the transcribed text, no explanations or commentary."));
    json_object_array_add(si_parts, si_part);
    json_object_object_add(system_instruction, "parts", si_parts);
    json_object_object_add(root, "system_instruction", system_instruction);

    free(base64_audio);

    const char *json_str = json_object_to_json_string(root);

    struct gemini_response response = {0};

    curl_easy_reset(ctx->curl_handle);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, gemini_write_callback);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, (void *)&response);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(ctx->curl_handle, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(ctx->curl_handle);
    curl_slist_free_all(headers);
    json_object_put(root);

    if (res != CURLE_OK) {
        fprintf(stderr, "Gemini transcription request failed: %s\n", curl_easy_strerror(res));
        if (response.data) free(response.data);
        return -1;
    }

    // Parse response
    if (response.data && response.size > 0) {
        struct json_object *resp_json = json_tokener_parse(response.data);
        if (resp_json) {
            // Gemini response structure: { candidates: [{ content: { parts: [{ text: "..." }] } }] }
            struct json_object *candidates;
            if (json_object_object_get_ex(resp_json, "candidates", &candidates) &&
                json_object_is_type(candidates, json_type_array) &&
                json_object_array_length(candidates) > 0) {

                struct json_object *first_candidate = json_object_array_get_idx(candidates, 0);
                struct json_object *content;
                if (json_object_object_get_ex(first_candidate, "content", &content)) {
                    struct json_object *parts;
                    if (json_object_object_get_ex(content, "parts", &parts) &&
                        json_object_is_type(parts, json_type_array) &&
                        json_object_array_length(parts) > 0) {

                        struct json_object *first_part = json_object_array_get_idx(parts, 0);
                        struct json_object *text_obj;
                        if (json_object_object_get_ex(first_part, "text", &text_obj)) {
                            *text = strdup(json_object_get_string(text_obj));
                            *text_len = strlen(*text);
                        }
                    }
                }
            }
            json_object_put(resp_json);
        }
        free(response.data);
    }

    return (*text != NULL) ? 0 : -1;
}

int vi_gemini_refine(vi_gemini_ctx_t *ctx, const char *input,
                     char **output, size_t *output_len) {
    if (!ctx || !input || !output || !output_len) return -1;

    *output = NULL;
    *output_len = 0;

    // Build prompt for refinement
    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
        "Format this voice dictation transcript. Apply these steps strictly in order:\n\n"
        "1. **Self-correction** — The speaker often interrupts themselves to correct a previous word. "
        "When you see this pattern, DELETE the wrong word and KEEP the corrected one:\n"
        "   - '...X. No, I mean Y.' => delete X, keep Y\n"
        "   - '...X. Sorry, Y.' => delete X, keep Y\n"
        "   - '...X. Wait, Y.' => delete X, keep Y\n"
        "   - '...X not Y' => keep Y, delete X\n"
        "   - Examples:\n"
        "     'meeting at 2pm no 3pm' => 'meeting at 3pm'\n"
        "     'Christina we have meeting tomorrow building 2pm no 3pm' => 'Christina, we have meeting tomorrow building 3pm'\n"
        "     'at the park sorry at the office' => 'at the office'\n\n"
        "2. **Autoformatting** — add punctuation (. , ? !) and fix capitalization (first letter uppercase).\n\n"
        "3. **Filler removal** — remove filler words: um, uh, like, you know, er, ah.\n\n"
        "Return ONLY the final clean text, nothing else:\n\n%s", input);

    char url[512];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/gemini-3-flash-preview:generateContent?key=%s",
             ctx->api_key);

    // Build JSON request
    struct json_object *root = json_object_new_object();
    struct json_object *contents = json_object_new_array();
    struct json_object *content = json_object_new_object();
    struct json_object *parts = json_object_new_array();
    struct json_object *part = json_object_new_object();

    json_object_object_add(part, "text", json_object_new_string(prompt));
    json_object_array_add(parts, part);
    json_object_object_add(content, "parts", parts);
    json_object_array_add(contents, content);
    json_object_object_add(root, "contents", contents);

    const char *json_str = json_object_to_json_string(root);

    struct gemini_response response = {0};

    curl_easy_reset(ctx->curl_handle);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEFUNCTION, gemini_write_callback);
    curl_easy_setopt(ctx->curl_handle, CURLOPT_WRITEDATA, (void *)&response);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(ctx->curl_handle, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(ctx->curl_handle);
    curl_slist_free_all(headers);
    json_object_put(root);

    if (res != CURLE_OK) {
        fprintf(stderr, "Gemini refinement request failed: %s\n", curl_easy_strerror(res));
        if (response.data) free(response.data);
        return -1;
    }

    // Parse response (same structure as transcribe)
    if (response.data && response.size > 0) {
        struct json_object *resp_json = json_tokener_parse(response.data);
        if (resp_json) {
            struct json_object *candidates;
            if (json_object_object_get_ex(resp_json, "candidates", &candidates) &&
                json_object_is_type(candidates, json_type_array) &&
                json_object_array_length(candidates) > 0) {

                struct json_object *first_candidate = json_object_array_get_idx(candidates, 0);
                struct json_object *content;
                if (json_object_object_get_ex(first_candidate, "content", &content)) {
                    struct json_object *parts;
                    if (json_object_object_get_ex(content, "parts", &parts) &&
                        json_object_is_type(parts, json_type_array) &&
                        json_object_array_length(parts) > 0) {

                        struct json_object *first_part = json_object_array_get_idx(parts, 0);
                        struct json_object *text_obj;
                        if (json_object_object_get_ex(first_part, "text", &text_obj)) {
                            *output = strdup(json_object_get_string(text_obj));
                            *output_len = strlen(*output);
                        }
                    }
                }
            }
            json_object_put(resp_json);
        }
        free(response.data);
    }

    return (*output != NULL) ? 0 : -1;
}

void vi_gemini_cleanup(vi_gemini_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->curl_handle) {
        curl_easy_cleanup(ctx->curl_handle);
        ctx->curl_handle = NULL;
    }
    // Note: curl_global_cleanup() should only be called once at program exit,
    // not per-instance. Global init/cleanup is handled in main.c.
}
