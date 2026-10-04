#include "memory.h"
#include "meta_text.h"

#include <ctype.h>
#include <string.h>

/** Fills `error` with a code and a message; does nothing when `error` is NULL. */
static void fail(edds_error *error, const char *code, const char *message) {
    if (error == NULL) {
        return;
    }

    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    (void)snprintf(error->message, sizeof error->message, "%s", message);
}

/**
 * Reads the whole file into one allocation with a NUL after its last byte; the caller releases it
 * with `edds_free`. Returns 0, with `error` filled, when the file is too large or cannot be read.
 */
int meta_read_text(FILE *input, char **source, size_t *size, edds_error *error) {
    long  length;
    char *allocation;

    *source = NULL;
    *size   = 0;

    /* The size, from a seek to the end and back to the start. */
    if (fseek(input, 0, SEEK_END) != 0 ||
        (length = ftell(input)) < 0 ||
        fseek(input, 0, SEEK_SET) != 0 ||
        (uint64_t)length > EDDS_MAX_FILE_BYTES) {
        fail(error, "metadata-size-limit", "The metadata could not be measured within the input limit.");
        return 0;
    }

    allocation = edds_alloc((size_t)length + 1u);

    if (allocation == NULL) {
        fail(error, "allocation-failed", "Memory for the metadata could not be allocated.");
        return 0;
    }

    if (fread(allocation, 1, (size_t)length, input) != (size_t)length) {
        edds_free(allocation);
        fail(error, "metadata-read-failed", "The metadata could not be read completely.");
        return 0;
    }

    allocation[length] = '\0';

    *source = allocation;
    *size   = (size_t)length;

    return 1;
}

/**
 * Moves past white space, line comments and block comments. Returns 0 when a block comment is not
 * closed before the end of the text.
 */
static int skip_space(meta_scanner *scan) {
    for (;;) {
        while (scan->at < scan->size && isspace((unsigned char)scan->source[scan->at])) {
            ++scan->at;
        }

        /* Only a slash with another character after it can start a comment. */
        if (scan->at + 1u >= scan->size || scan->source[scan->at] != '/') {
            return 1;
        }

        if (scan->source[scan->at + 1u] == '/') {
            /* A line comment runs to the end of its line. */
            scan->at += 2;

            while (scan->at < scan->size && scan->source[scan->at] != '\n') {
                ++scan->at;
            }
        } else if (scan->source[scan->at + 1u] == '*') {
            /* A block comment runs to the star and slash that close it. */
            scan->at += 2;

            while (scan->at + 1u < scan->size && !(scan->source[scan->at] == '*' && scan->source[scan->at + 1u] == '/')) {
                ++scan->at;
            }

            if (scan->at + 1u >= scan->size) {
                return 0;
            }

            scan->at += 2;
        } else {
            return 1;
        }
    }
}

/**
 * Reads the next token after any white space and comments, and moves past it. At the end of the
 * text the token is `META_TOKEN_END`; text that cannot be read gives `META_TOKEN_INVALID`.
 */
meta_token meta_next_token(meta_scanner *scan) {
    meta_token result;
    size_t     length = 0;

    memset(&result, 0, sizeof result);

    if (!skip_space(scan)) {
        result.kind = META_TOKEN_INVALID;
        return result;
    }

    if (scan->at == scan->size) {
        result.kind = META_TOKEN_END;
        return result;
    }

    /* The tokens of one character. */
    if (scan->source[scan->at] == '{') {
        ++scan->at;
        result.kind = META_TOKEN_OPEN;
        return result;
    }

    if (scan->source[scan->at] == '}') {
        ++scan->at;
        result.kind = META_TOKEN_CLOSE;
        return result;
    }

    if (scan->source[scan->at] == ':') {
        ++scan->at;
        result.kind = META_TOKEN_COLON;
        return result;
    }

    /*
     * A string runs to its closing quote; a backslash followed by a backslash or a quote stands
     * for that one character. A control character, or a string too long for `text`, is invalid.
     */
    if (scan->source[scan->at] == '"') {
        ++scan->at;
        result.kind = META_TOKEN_STRING;

        while (scan->at < scan->size && scan->source[scan->at] != '"') {
            char value = scan->source[scan->at++];

            if (value == '\\' && scan->at < scan->size && (scan->source[scan->at] == '\\' || scan->source[scan->at] == '"')) {
                value = scan->source[scan->at++];
            }

            if (length + 1u >= sizeof result.text || (unsigned char)value < 0x20u) {
                result.kind = META_TOKEN_INVALID;
                return result;
            }

            result.text[length++] = value;
        }

        /* The text ended before the closing quote. */
        if (scan->at >= scan->size) {
            result.kind = META_TOKEN_INVALID;
            return result;
        }

        ++scan->at;
        result.text[length] = '\0';
        return result;
    }

    /* Anything else is a word, up to white space, a brace, a colon or a quote. */
    result.kind = META_TOKEN_WORD;

    while (scan->at < scan->size &&
        !isspace((unsigned char)scan->source[scan->at]) &&
        scan->source[scan->at] != '{' &&
        scan->source[scan->at] != '}' &&
        scan->source[scan->at] != ':' &&
        scan->source[scan->at] != '"') {
        if (length + 1u >= sizeof result.text) {
            result.kind = META_TOKEN_INVALID;
            return result;
        }

        result.text[length++] = scan->source[scan->at++];
    }

    if (length == 0) {
        result.kind = META_TOKEN_INVALID;
        return result;
    }

    result.text[length] = '\0';

    return result;
}

/**
 * Skips to the brace that closes a block whose opening brace was just read, counting the blocks
 * nested inside it. Returns 0 when the text ends, or holds a token that cannot be read, before it.
 */
int meta_skip_open_block(meta_scanner *scan) {
    unsigned depth = 1;

    while (depth != 0) {
        const meta_token value = meta_next_token(scan);

        if (value.kind == META_TOKEN_OPEN) {
            ++depth;
        } else if (value.kind == META_TOKEN_CLOSE) {
            --depth;
        } else if (value.kind == META_TOKEN_END || value.kind == META_TOKEN_INVALID) {
            return 0;
        }
    }

    return 1;
}

/** Copies `source` with its NUL into `destination`, or returns 0 when it does not fit. */
int meta_copy_text(char *destination, size_t capacity, const char *source) {
    const size_t size = strlen(source);

    if (size >= capacity) {
        return 0;
    }

    memcpy(destination, source, size + 1u);

    return 1;
}

/** Whether `text` is exactly sixteen hexadecimal digits, the way a GUID is written. */
int meta_valid_guid(const char *text) {
    size_t at = 0;

    for (; text[at] != '\0'; ++at) {
        if (at == 16u || !isxdigit((unsigned char)text[at])) {
            return 0;
        }
    }

    return at == 16u;
}

/**
 * Splits `{GUID}path` into the sixteen hexadecimal digits, copied into `guid` as written, and the
 * resource path after the closing brace, copied into `name`. Returns 0 when `value` is not of that
 * shape or the path does not fit.
 */
int meta_parse_name(const char *value, char guid[EDDS_METADATA_GUID_BYTES], char *name, size_t name_capacity) {
    char        digits[EDDS_METADATA_GUID_BYTES];
    const char *close;

    /* An opening brace, sixteen characters, the first closing brace, and at least one more. */
    if (value[0] != '{' || (close = strchr(value, '}')) == NULL || close - value != 17 || close[1] == '\0') {
        return 0;
    }

    memcpy(digits, value + 1, 16);
    digits[16] = '\0';

    if (!meta_valid_guid(digits)) {
        return 0;
    }

    memcpy(guid, digits, sizeof digits);

    return meta_copy_text(name, name_capacity, close + 1);
}
