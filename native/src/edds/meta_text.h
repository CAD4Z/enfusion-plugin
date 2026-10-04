/*
 * The text of an Enfusion `.meta` file: words, quoted strings, braces, the `:` of platform
 * inheritance, and line and block comments. A texture recipe and a font recipe are both written in
 * it, so both read it through this one scanner.
 */
#ifndef ENFUSION_META_TEXT_H
#define ENFUSION_META_TEXT_H

#include <edds/edds.h>

#include <stddef.h>
#include <stdio.h>

typedef enum meta_token_kind {
    META_TOKEN_END,
    META_TOKEN_WORD,
    META_TOKEN_STRING,
    META_TOKEN_OPEN,
    META_TOKEN_CLOSE,
    META_TOKEN_COLON,
    META_TOKEN_INVALID
} meta_token_kind;

typedef struct meta_token {
    meta_token_kind kind;
    char            text[EDDS_METADATA_PATH_BYTES];
} meta_token;

typedef struct meta_scanner {
    const char *source;
    size_t      size;
    size_t      at;
} meta_scanner;

/** The whole file as one NUL-terminated allocation, released with `edds_free`. */
int meta_read_text(FILE *input, char **source, size_t *size, edds_error *error);

meta_token meta_next_token(meta_scanner *scan);

/** Skips to the brace that closes a block whose opening brace was just read. */
int meta_skip_open_block(meta_scanner *scan);

int meta_copy_text(char *destination, size_t capacity, const char *source);

/** Whether `text` is exactly sixteen hexadecimal digits, the way a GUID is written. */
int meta_valid_guid(const char *text);

/**
 * `Name "{GUID}path"`: exactly sixteen hexadecimal digits in braces, then a non-empty resource
 * path. The GUID is copied character for character, case included.
 */
int meta_parse_name(
    const char *value,
    char        guid[EDDS_METADATA_GUID_BYTES],
    char       *name,
    size_t      name_capacity);

#endif
