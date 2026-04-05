#include "voiceinput.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

static const char *injection_method_names[] = {
    [VI_INJECT_CLIPBOARD] = "clipboard",
    [VI_INJECT_LIBEI] = "libei",
    [VI_INJECT_WTYPE] = "wtype",
    [VI_INJECT_YDOTOOL] = "ydotool"
};

static const char *provider_names[] = {
    [VI_PROVIDER_GEMINI] = "gemini"
};

void vi_config_defaults(vi_config_t *config) {
    if (!config) return;

    memset(config, 0, sizeof(vi_config_t));

    config->provider = VI_PROVIDER_GEMINI;

    // Auto-detect GEMINI_API_KEY from environment
    const char *env_api_key = getenv("GEMINI_API_KEY");
    if (env_api_key && strlen(env_api_key) > 0) {
        strncpy(config->gemini_api_key, env_api_key, VI_MAX_API_KEY_LEN - 1);
    }

    // Hotkey defaults
    strncpy(config->hotkey_modifier, VI_DEFAULT_HOTKEY_MODIFIER, VI_MAX_HOTKEY_NAME - 1);
    strncpy(config->hotkey_key, VI_DEFAULT_HOTKEY_KEY, VI_MAX_HOTKEY_NAME - 1);

    // Audio defaults
    config->sample_rate = VI_DEFAULT_SAMPLE_RATE;
    config->channels = VI_DEFAULT_CHANNELS;
    config->chunk_duration_ms = VI_DEFAULT_CHUNK_MS;

    // Injection defaults
    config->injection_method = VI_INJECT_LIBEI;
    config->injection_fallback = true;

    // Refinement defaults
    config->refinement_enabled = true;
    config->remove_fillers = true;
    config->auto_punctuate = true;

    // History defaults
    config->history_enabled = true;
    config->history_max_entries = 1000;
}

static char *get_home_dir(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    return home ? strdup(home) : NULL;
}

static char *get_config_path(void) {
    char *home = get_home_dir();
    if (!home) return NULL;

    char *path = malloc(VI_MAX_PATH);
    if (!path) {
        free(home);
        return NULL;
    }

    snprintf(path, VI_MAX_PATH, "%s/%s/%s", home, VI_CONFIG_DIR, VI_CONFIG_FILE);
    free(home);
    return path;
}

int vi_config_load(const char *path, vi_config_t *config) {
    if (!config) return -1;

    // Start with defaults
    vi_config_defaults(config);

    char *config_path = path ? strdup(path) : get_config_path();
    if (!config_path) return -1;

    FILE *f = fopen(config_path, "r");
    if (!f) {
        free(config_path);
        return -1; // Config file doesn't exist, use defaults
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json_str = malloc(fsize + 1);
    if (!json_str) {
        fclose(f);
        free(config_path);
        return -1;
    }

    size_t bytes_read = fread(json_str, 1, fsize, f);
    json_str[bytes_read] = '\0';
    fclose(f);

    // Parse JSON using json-c
    struct json_object *root = json_tokener_parse(json_str);
    free(json_str);

    if (!root) {
        free(config_path);
        return -1;
    }

    // Parse provider settings
    struct json_object *provider;
    if (json_object_object_get_ex(root, "provider", &provider)) {
        const char *provider_str = json_object_get_string(provider);
        if (strcmp(provider_str, "gemini") == 0) {
            config->provider = VI_PROVIDER_GEMINI;
        }
    }

    // Parse Gemini settings
    struct json_object *gemini;
    if (json_object_object_get_ex(root, "gemini", &gemini)) {
        struct json_object *api_key;
        if (json_object_object_get_ex(gemini, "api_key", &api_key)) {
            const char *key = json_object_get_string(api_key);
            // Only use config value if not empty and not placeholder
            if (key && strlen(key) > 0 &&
                strcmp(key, "YOUR_GEMINI_API_KEY_HERE") != 0 &&
                strcmp(key, "YOUR_API_KEY") != 0) {
                strncpy(config->gemini_api_key, key, VI_MAX_API_KEY_LEN - 1);
            }
        }
    }

    // Environment variable overrides config file
    const char *env_api_key = getenv("GEMINI_API_KEY");
    if (env_api_key && strlen(env_api_key) > 0) {
        strncpy(config->gemini_api_key, env_api_key, VI_MAX_API_KEY_LEN - 1);
    }

    // Parse hotkey settings
    struct json_object *hotkey;
    if (json_object_object_get_ex(root, "hotkey", &hotkey)) {
        struct json_object *modifier;
        if (json_object_object_get_ex(hotkey, "modifier", &modifier)) {
            strncpy(config->hotkey_modifier, json_object_get_string(modifier), VI_MAX_HOTKEY_NAME - 1);
        }

        struct json_object *key;
        if (json_object_object_get_ex(hotkey, "key", &key)) {
            strncpy(config->hotkey_key, json_object_get_string(key), VI_MAX_HOTKEY_NAME - 1);
        }
    }

    // Parse audio settings
    struct json_object *audio;
    if (json_object_object_get_ex(root, "audio", &audio)) {
        struct json_object *sr;
        if (json_object_object_get_ex(audio, "sample_rate", &sr)) {
            config->sample_rate = json_object_get_int(sr);
        }

        struct json_object *ch;
        if (json_object_object_get_ex(audio, "channels", &ch)) {
            config->channels = json_object_get_int(ch);
        }

        struct json_object *dur;
        if (json_object_object_get_ex(audio, "chunk_duration_ms", &dur)) {
            config->chunk_duration_ms = json_object_get_int(dur);
        }
    }

    // Parse text injection settings
    struct json_object *injection;
    if (json_object_object_get_ex(root, "text_injection", &injection)) {
        struct json_object *method;
        if (json_object_object_get_ex(injection, "method", &method)) {
            const char *method_str = json_object_get_string(method);
            if (strcmp(method_str, "wtype") == 0) {
                config->injection_method = VI_INJECT_WTYPE;
            } else if (strcmp(method_str, "ydotool") == 0) {
                config->injection_method = VI_INJECT_YDOTOOL;
            } else {
                config->injection_method = VI_INJECT_LIBEI;
            }
        }

        struct json_object *fallback;
        if (json_object_object_get_ex(injection, "fallback", &fallback)) {
            config->injection_fallback = json_object_get_boolean(fallback);
        }
    }

    // Parse refinement settings
    struct json_object *refinement;
    if (json_object_object_get_ex(root, "refinement", &refinement)) {
        struct json_object *enabled;
        if (json_object_object_get_ex(refinement, "enabled", &enabled)) {
            config->refinement_enabled = json_object_get_boolean(enabled);
        }

        struct json_object *fillers;
        if (json_object_object_get_ex(refinement, "remove_fillers", &fillers)) {
            config->remove_fillers = json_object_get_boolean(fillers);
        }

        struct json_object *punct;
        if (json_object_object_get_ex(refinement, "auto_punctuate", &punct)) {
            config->auto_punctuate = json_object_get_boolean(punct);
        }
    }

    // Parse history settings
    struct json_object *history;
    if (json_object_object_get_ex(root, "history", &history)) {
        struct json_object *enabled;
        if (json_object_object_get_ex(history, "enabled", &enabled)) {
            config->history_enabled = json_object_get_boolean(enabled);
        }

        struct json_object *max;
        if (json_object_object_get_ex(history, "max_entries", &max)) {
            config->history_max_entries = json_object_get_int(max);
        }
    }

    json_object_put(root);
    free(config_path);

    return 0;
}

int vi_config_save(const vi_config_t *config, const char *path) {
    if (!config) return -1;

    char *config_path = path ? strdup(path) : get_config_path();
    if (!config_path) return -1;

    // Create directory if it doesn't exist
    char *dir = strdup(config_path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0755);
    }
    free(dir);

    // Build JSON using json-c
    struct json_object *root = json_object_new_object();

    json_object_object_add(root, "provider", json_object_new_string(provider_names[config->provider]));

    struct json_object *gemini = json_object_new_object();
    json_object_object_add(gemini, "api_key", json_object_new_string(config->gemini_api_key));
    json_object_object_add(root, "gemini", gemini);

    struct json_object *hotkey = json_object_new_object();
    json_object_object_add(hotkey, "modifier", json_object_new_string(config->hotkey_modifier));
    json_object_object_add(hotkey, "key", json_object_new_string(config->hotkey_key));
    json_object_object_add(root, "hotkey", hotkey);

    struct json_object *audio = json_object_new_object();
    json_object_object_add(audio, "sample_rate", json_object_new_int(config->sample_rate));
    json_object_object_add(audio, "channels", json_object_new_int(config->channels));
    json_object_object_add(audio, "chunk_duration_ms", json_object_new_int(config->chunk_duration_ms));
    json_object_object_add(root, "audio", audio);

    struct json_object *injection = json_object_new_object();
    json_object_object_add(injection, "method", json_object_new_string(injection_method_names[config->injection_method]));
    json_object_object_add(injection, "fallback", json_object_new_boolean(config->injection_fallback));
    json_object_object_add(root, "text_injection", injection);

    struct json_object *refinement = json_object_new_object();
    json_object_object_add(refinement, "enabled", json_object_new_boolean(config->refinement_enabled));
    json_object_object_add(refinement, "remove_fillers", json_object_new_boolean(config->remove_fillers));
    json_object_object_add(refinement, "auto_punctuate", json_object_new_boolean(config->auto_punctuate));
    json_object_object_add(root, "refinement", refinement);

    struct json_object *history = json_object_new_object();
    json_object_object_add(history, "enabled", json_object_new_boolean(config->history_enabled));
    json_object_object_add(history, "max_entries", json_object_new_int(config->history_max_entries));
    json_object_object_add(root, "history", history);

    const char *json_str = json_object_to_json_string(root);

    FILE *f = fopen(config_path, "w");
    if (!f) {
        json_object_put(root);
        free(config_path);
        return -1;
    }

    fputs(json_str, f);
    fclose(f);

    json_object_put(root);
    free(config_path);

    return 0;
}

int vi_config_init(vi_config_t *config) {
    return vi_config_load(NULL, config);
}
