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

/**
 * The kinds of token: the end of the text, a word, a quoted string, an opening or closing brace,
 * a colon, and text the scanner cannot read.
 */
typedef enum meta_token_kind {
    META_TOKEN_END,
    META_TOKEN_WORD,
    META_TOKEN_STRING,
    META_TOKEN_OPEN,
    META_TOKEN_CLOSE,
    META_TOKEN_COLON,
    META_TOKEN_INVALID
} meta_token_kind;

/** One token: its kind, and the text of a word or of a string without its quotes and escapes. */
typedef struct meta_token {
    meta_token_kind kind;
    char            text[EDDS_METADATA_PATH_BYTES];
} meta_token;

/** The text being scanned, its size, and where the next token starts. */
typedef struct meta_scanner {
    const char *source;
    size_t      size;
    size_t      at;
} meta_scanner;

/**
 * The whole file as one NUL-terminated allocation, released with `edds_free`. Returns 0, with
 * `error` filled, when the file is too large or cannot be read.
 */
int meta_read_text(FILE *input, char **source, size_t *size, edds_error *error);

/** The next token after any white space and comments; `META_TOKEN_END` at the end of the text. */
meta_token meta_next_token(meta_scanner *scan);

/**
 * Skips to the brace that closes a block whose opening brace was just read. Returns 0 when the
 * text ends, or holds a token that cannot be read, before that brace.
 */
int meta_skip_open_block(meta_scanner *scan);

/** Copies `source` with its NUL into `destination`, or returns 0 when it does not fit. */
int meta_copy_text(char *destination, size_t capacity, const char *source);

/** Whether `text` is exactly sixteen hexadecimal digits, the way a GUID is written. */
int meta_valid_guid(const char *text);

/**
 * `Name "{GUID}path"`: exactly sixteen hexadecimal digits in braces, then a non-empty resource
 * path. The GUID is copied character for character, case included. Returns 0 when `value` is not
 * of that shape or the path does not fit `name`.
 */
int meta_parse_name(const char *value, char guid[EDDS_METADATA_GUID_BYTES], char *name, size_t name_capacity);

#endif
