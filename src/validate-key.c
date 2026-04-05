/**
 * Validate Gemini API Key
 * Simple tool to test if the API key works
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>

struct response_buffer {
    char *data;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct response_buffer *mem = (struct response_buffer *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

int validate_api_key(const char *api_key) {
    if (!api_key || strlen(api_key) == 0) {
        fprintf(stderr, "❌ API key is empty\n");
        return 1;
    }

    printf("Testing API key: %.8s...\n", api_key);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "❌ Failed to initialize libcurl\n");
        return 1;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/gemini-3-flash-preview:generateContent?key=%s",
             api_key);

    const char *json_payload = "{\"contents\":[{\"parts\":[{\"text\":\"say hello\"}]}]}";

    struct response_buffer response = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    printf("Sending request to Gemini API...\n");
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        fprintf(stderr, "❌ Request failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return 1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    printf("HTTP Response: %ld\n", http_code);

    if (response.data && response.size > 0) {
        struct json_object *resp_json = json_tokener_parse(response.data);
        if (resp_json) {
            // Check for error
            struct json_object *error;
            if (json_object_object_get_ex(resp_json, "error", &error)) {
                struct json_object *message;
                if (json_object_object_get_ex(error, "message", &message)) {
                    fprintf(stderr, "❌ API Error: %s\n", json_object_get_string(message));
                } else {
                    fprintf(stderr, "❌ API Error (unknown)\n");
                }
                json_object_put(resp_json);
                free(response.data);
                return 1;
            }

            // Check for successful response
            struct json_object *candidates;
            if (json_object_object_get_ex(resp_json, "candidates", &candidates) &&
                json_object_is_type(candidates, json_type_array) &&
                json_object_array_length(candidates) > 0) {

                struct json_object *first = json_object_array_get_idx(candidates, 0);
                struct json_object *content, *parts;
                if (json_object_object_get_ex(first, "content", &content) &&
                    json_object_object_get_ex(content, "parts", &parts) &&
                    json_object_array_length(parts) > 0) {

                    struct json_object *text_obj = json_object_array_get_idx(parts, 0);
                    struct json_object *text;
                    if (json_object_object_get_ex(text_obj, "text", &text)) {
                        printf("✅ API key is valid!\n");
                        printf("   Response: %s\n", json_object_get_string(text));
                        json_object_put(resp_json);
                        free(response.data);
                        return 0;
                    }
                }
            }

            json_object_put(resp_json);
        }

        // Print raw response for debugging
        printf("Raw response: %s\n", response.data);
    }

    free(response.data);
    fprintf(stderr, "❌ Invalid API key or unexpected response\n");
    return 1;
}

int main(int argc, char *argv[]) {
    const char *api_key = getenv("GEMINI_API_KEY");

    if (!api_key) {
        // Try to read from config
        const char *home = getenv("HOME");
        if (home) {
            char config_path[512];
            snprintf(config_path, sizeof(config_path), "%s/.config/voiceinput/config.json", home);
            FILE *f = fopen(config_path, "r");
            if (f) {
                fclose(f);
                // Config reading would go here
            }
        }
        fprintf(stderr, "❌ GEMINI_API_KEY not set in environment\n");
        fprintf(stderr, "   Set it in /etc/environment or run:\n");
        fprintf(stderr, "   export GEMINI_API_KEY='your-key-here'\n");
        return 1;
    }

    return validate_api_key(api_key);
}
