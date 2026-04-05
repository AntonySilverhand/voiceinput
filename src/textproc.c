#include "voiceinput.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// ============================================================================
// Filler Word Removal
// ============================================================================

static const char *filler_words[] = {
    "um", "uh", "er", "erm",
    "like", "you know", "so", "well",
    "actually", "basically", "literally",
    "i mean", "sort of", "kind of",
    "right", "okay", "yeah", "yes", "no",
    NULL
};

int vi_textproc_remove_fillers(char *text) {
    if (!text || strlen(text) == 0) return -1;

    // Simple approach: remove common filler words
    // This is a basic implementation - production would use more sophisticated NLP

    char *result = malloc(strlen(text) + 1);
    if (!result) return -1;

    char *dst = result;
    char *src = text;
    char lower_word[64];   // lowercased copy for comparison
    char *word_begin = NULL; // pointer to start of word in original text
    int in_word = 0;
    int word_len = 0;

    while (*src) {
        if (isspace(*src)) {
            if (in_word && word_len > 0) {
                lower_word[word_len] = '\0';

                // Check if this is a filler word
                int is_filler = 0;
                for (int i = 0; filler_words[i] != NULL; i++) {
                    if (strcasecmp(lower_word, filler_words[i]) == 0) {
                        is_filler = 1;
                        break;
                    }
                }

                if (!is_filler) {
                    // Copy original-cased word to result
                    if (dst > result && *(dst - 1) != ' ') {
                        *dst++ = ' ';
                    }
                    memcpy(dst, word_begin, word_len);
                    dst += word_len;
                }

                in_word = 0;
                word_len = 0;
            }

            // Copy whitespace (normalized to single space)
            if (dst > result && *(dst - 1) != ' ') {
                *dst++ = ' ';
            }

            src++;
        } else {
            if (!in_word) {
                in_word = 1;
                word_len = 0;
                word_begin = src;
            }
            if (word_len < 63) {
                lower_word[word_len++] = tolower(*src);
            }
            src++;
        }
    }

    // Handle last word
    if (in_word && word_len > 0) {
        lower_word[word_len] = '\0';

        int is_filler = 0;
        for (int i = 0; filler_words[i] != NULL; i++) {
            if (strcasecmp(lower_word, filler_words[i]) == 0) {
                is_filler = 1;
                break;
            }
        }

        if (!is_filler) {
            if (dst > result && *(dst - 1) != ' ') {
                *dst++ = ' ';
            }
            memcpy(dst, word_begin, word_len);
            dst += word_len;
        }
    }

    *dst = '\0';

    // Trim trailing space
    while (dst > result && *(dst - 1) == ' ') {
        dst--;
    }
    *dst = '\0';

    // Copy result back to original
    strcpy(text, result);
    free(result);

    return 0;
}

// ============================================================================
// Whitespace Trimming
// ============================================================================

int vi_textproc_trim_whitespace(char *text) {
    if (!text || strlen(text) == 0) return -1;

    // Trim leading whitespace
    char *start = text;
    while (isspace(*start)) start++;

    // Trim trailing whitespace
    char *end = start + strlen(start) - 1;
    while (end > start && isspace(*end)) end--;
    *(end + 1) = '\0';

    // Move trimmed text to beginning
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    // Normalize internal whitespace
    char *dst = text;
    char *src = text;
    int in_space = 0;

    while (*src) {
        if (isspace(*src)) {
            if (!in_space) {
                *dst++ = ' ';
                in_space = 1;
            }
        } else {
            *dst++ = *src;
            in_space = 0;
        }
        src++;
    }

    *dst = '\0';

    return 0;
}

// ============================================================================
// Simple Auto-Punctuation (Rule-based)
// ============================================================================

int vi_textproc_auto_punctuate(char **text) {
    if (!text || !*text || strlen(*text) == 0) return -1;

    // Capitalize first letter
    if (islower((*text)[0])) {
        (*text)[0] = toupper((*text)[0]);
    }

    // Add period at end if no punctuation
    size_t len = strlen(*text);
    if (len > 0 && (*text)[len - 1] != '.' &&
        (*text)[len - 1] != '?' && (*text)[len - 1] != '!' &&
        (*text)[len - 1] != ',' && (*text)[len - 1] != ';') {
        char *expanded = realloc(*text, len + 2);
        if (!expanded) return -1;
        *text = expanded;
        (*text)[len] = '.';
        (*text)[len + 1] = '\0';
    }

    return 0;
}
