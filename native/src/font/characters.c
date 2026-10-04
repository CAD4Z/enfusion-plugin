/* The characters a font is made for: read from a UTF-8 character-set file, or the built-in set. */
#include "font_internal.h"

#include <stdlib.h>
#include <string.h>

/** Characters that only separate the others: control characters, the space, line separators. */
static int separates(uint32_t code) {
    return code < 0x20u || code == 0x20u || code == 0x7Fu || (code >= 0x80u && code <= 0x9Fu) || code == 0x2028u || code == 0x2029u;
}

/**
 * One UTF-8 scalar value; 0 at a byte sequence that is not one. Otherwise the value goes to *code
 * and the number of bytes it took is returned.
 */
static size_t decode(const uint8_t *text, size_t size, uint32_t *code) {
    const uint8_t lead = text[0];
    size_t        length;
    uint32_t      value;
    uint32_t      smallest;

    /* A byte below 0x80 is a character on its own. */
    if (lead < 0x80u) {
        *code = lead;
        return 1;
    }

    /*
     * The lead byte of a longer sequence: how many bytes it takes, the top bits of the value, and
     * the smallest value that needs that many bytes.
     */
    if ((lead & 0xE0u) == 0xC0u) {
        length   = 2;
        value    = lead & 0x1Fu;
        smallest = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
        length   = 3;
        value    = lead & 0x0Fu;
        smallest = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
        length   = 4;
        value    = lead & 0x07u;
        smallest = 0x10000u;
    } else {
        return 0;
    }

    if (length > size) {
        return 0;
    }

    /* Every following byte must be 10xxxxxx, and adds its low six bits to the value. */
    for (size_t at = 1; at < length; ++at) {
        if ((text[at] & 0xC0u) != 0x80u) {
            return 0;
        }

        value = (value << 6) | (text[at] & 0x3Fu);
    }

    /* Refused: a value written in more bytes than it needs, one above 0x10FFFF, or a surrogate. */
    if (value < smallest || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
        return 0;
    }

    *code = value;

    return length;
}

/** The qsort comparison for code points in ascending order: -1, 0 or 1. */
static int ascending(const void *left, const void *right) {
    const uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;

    return (a > b) - (a < b);
}

/**
 * Sorts the code points, drops the repeats and stores them in `characters`, which from then on
 * owns `codes`. More distinct characters than FONT_MAX_CHARACTERS are refused, and `codes` is
 * freed.
 */
static edds_status finish(uint32_t *codes, size_t count, font_characters *characters, edds_error *error) {
    size_t unique = 0;

    qsort(codes, count, sizeof *codes, ascending);

    /* Once sorted, repeats sit side by side: each value is kept once, packed to the front. */
    for (size_t at = 0; at < count; ++at) {
        if (unique == 0 || codes[unique - 1] != codes[at]) {
            codes[unique++] = codes[at];
        }
    }

    if (unique > FONT_MAX_CHARACTERS) {
        free(codes);
        font_fail(error, "character-set-limit", "A font holds at most %u characters.", FONT_MAX_CHARACTERS);
        return EDDS_UNSUPPORTED_FORMAT;
    }

    characters->codes = codes;
    characters->count = unique;

    return EDDS_OK;
}

/**
 * Reads a character-set file: every UTF-8 character in it that does not only separate the others
 * joins the set. On success the caller owns `characters` and frees it with font_characters_free.
 */
edds_status font_characters_parse(const uint8_t *text, size_t size, font_characters *characters, edds_error *error) {
    uint32_t *codes;
    size_t    count = 0;
    size_t    at    = 0;

    if (characters == NULL || (text == NULL && size != 0)) {
        font_fail(error, "invalid-api-argument", "The character text and set are required.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* The set starts out empty. */
    memset(characters, 0, sizeof *characters);

    if (size > FONT_MAX_CHARACTER_FILE_BYTES) {
        font_fail(error, "character-file-limit", "A character set file is at most %u bytes.", FONT_MAX_CHARACTER_FILE_BYTES);
        return EDDS_INVALID_INPUT;
    }

    /* Room for one code point per byte of text, and at least one. */
    codes = malloc((size == 0 ? 1u : size) * sizeof *codes);

    if (codes == NULL) {
        font_fail(error, "allocation-failed", "Memory for the character set could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* A UTF-8 byte order mark at the start is skipped. */
    if (size >= 3u && text[0] == 0xEFu && text[1] == 0xBBu && text[2] == 0xBFu) {
        at = 3;
    }

    /* The characters one by one; the ones that only separate are left out. */
    while (at < size) {
        uint32_t     code   = 0;
        const size_t length = decode(text + at, size - at, &code);

        if (length == 0) {
            free(codes);
            font_fail(error, "malformed-characters", "The character set is not valid UTF-8 at byte %llu.", (unsigned long long)at);
            return EDDS_INVALID_INPUT;
        }

        if (!separates(code)) {
            codes[count++] = code;
        }

        at += length;
    }

    return finish(codes, count, characters, error);
}

/**
 * The built-in set: every code point of three ranges, Basic Latin, Latin-1 and Cyrillic. On
 * success the caller owns `characters` and frees it with font_characters_free.
 */
edds_status font_characters_builtin(font_characters *characters, edds_error *error) {
    /* The first and last code point of each range. */
    static const uint32_t ranges[][2] = { { 0x20u, 0x7Eu }, { 0xA0u, 0xFFu }, { 0x400u, 0x45Fu } };

    uint32_t *codes = malloc(512u * sizeof *codes);
    size_t    count = 0;

    if (characters == NULL) {
        free(codes);
        font_fail(error, "invalid-api-argument", "The character set is required.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* The set starts out empty. */
    memset(characters, 0, sizeof *characters);

    if (codes == NULL) {
        font_fail(error, "allocation-failed", "Memory for the character set could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }

    for (size_t range = 0; range < sizeof ranges / sizeof ranges[0]; ++range) {
        for (uint32_t code = ranges[range][0]; code <= ranges[range][1]; ++code) {
            codes[count++] = code;
        }
    }

    return finish(codes, count, characters, error);
}

/** Frees the code points of a set and leaves it empty; a NULL set is left alone. */
void font_characters_free(font_characters *characters) {
    if (characters == NULL) {
        return;
    }

    free(characters->codes);
    memset(characters, 0, sizeof *characters);
}
