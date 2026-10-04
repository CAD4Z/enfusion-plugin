/*
 * The batch protocol: every line is one JSON object, a record whose "kind" is "batch" (the
 * header), "job" or "end". JSON is read here only as far as these records need it, and the
 * reader at the end of the file frames the lines out of a byte stream.
 */
#include <edds/batch.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** Where reading is in a line, and where the line ends. */
typedef struct json_scan {
    const char *at;
    const char *end;
} json_scan;

/** Fills `error` with a code and a printf-style message; returns EDDS_INVALID_INVOCATION. */
static edds_status fail(edds_error *error, const char *code, const char *format, ...) {
    va_list arguments;

    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);

    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof error->message, format, arguments);
    va_end(arguments);

    return EDDS_INVALID_INVOCATION;
}

/** Steps over the spaces, tabs and line breaks JSON allows between its parts. */
static void space(json_scan *scan) {
    while (scan->at < scan->end &&
        (*scan->at == ' ' || *scan->at == '\t' || *scan->at == '\r' || *scan->at == '\n')) {
        ++scan->at;
    }
}

/** Steps over the character `value`, after any whitespace; 0 when something else comes next. */
static int take(json_scan *scan, char value) {
    space(scan);

    if (scan->at == scan->end || *scan->at != value) {
        return 0;
    }

    ++scan->at;

    return 1;
}

/** Steps over the word `value` (true, false or null) after any whitespace; 0 if it is not there. */
static int literal(json_scan *scan, const char *value) {
    const size_t size = strlen(value);

    space(scan);

    if ((size_t)(scan->end - scan->at) < size || memcmp(scan->at, value, size) != 0) {
        return 0;
    }

    scan->at += size;

    return 1;
}

/** Puts the value of the hexadecimal digit `value` in `*digit`; 0 when it is not one. */
static int hex(char value, uint32_t *digit) {
    if (value >= '0' && value <= '9') {
        *digit = (uint32_t)(value - '0');
    } else if (value >= 'a' && value <= 'f') {
        *digit = (uint32_t)(value - 'a') + 10u;
    } else if (value >= 'A' && value <= 'F') {
        *digit = (uint32_t)(value - 'A') + 10u;
    } else {
        return 0;
    }

    return 1;
}

/** Adds one byte to a string being read, keeping room for its NUL; 0 when it is full. */
static int append_byte(char *output, size_t capacity, size_t *size, uint8_t value) {
    if (*size + 1u >= capacity) {
        return 0;
    }

    output[(*size)++] = (char)value;

    return 1;
}

/**
 * Adds the code point of a four-digit escape as UTF-8: one byte up to 7F, two up to 7FF, three
 * above. A surrogate (D800 to DFFF) is refused (0), as is a string that is full.
 */
static int append_codepoint(char *output, size_t capacity, size_t *size, uint32_t value) {
    if (value <= 0x7fu) {
        return append_byte(output, capacity, size, (uint8_t)value);
    }

    if (value <= 0x7ffu) {
        return append_byte(output, capacity, size, (uint8_t)(0xc0u | (value >> 6u))) &&
            append_byte(output, capacity, size, (uint8_t)(0x80u | (value & 0x3fu)));
    }

    if (value >= 0xd800u && value <= 0xdfffu) {
        return 0;
    }

    return append_byte(output, capacity, size, (uint8_t)(0xe0u | (value >> 12u))) &&
        append_byte(output, capacity, size, (uint8_t)(0x80u | ((value >> 6u) & 0x3fu))) &&
        append_byte(output, capacity, size, (uint8_t)(0x80u | (value & 0x3fu)));
}

/**
 * Reads a JSON string into `output`, its escapes undone and a NUL after it. Returns 0 when it is
 * malformed or does not fit `capacity` bytes, the NUL included.
 */
static int string_value(json_scan *scan, char *output, size_t capacity) {
    size_t size = 0;

    if (!take(scan, '"') || capacity == 0u) {
        return 0;
    }

    /* Byte by byte up to the closing quote. A raw control character (below 0x20) is refused. */
    while (scan->at < scan->end && *scan->at != '"') {
        unsigned char value = (unsigned char)*scan->at++;

        if (value < 0x20u) {
            return 0;
        }

        /* An escape: the character after the backslash says what it stands for. */
        if (value == '\\') {
            uint32_t codepoint = 0;

            if (scan->at == scan->end) {
                return 0;
            }

            value = (unsigned char)*scan->at++;

            switch (value) {
                case '"':
                case '\\':
                case '/':  break;
                case 'b':  value = '\b'; break;
                case 'f':  value = '\f'; break;
                case 'n':  value = '\n'; break;
                case 'r':  value = '\r'; break;
                case 't':  value = '\t'; break;

                /* `u` and four hexadecimal digits: a code point, added as UTF-8. */
                case 'u':
                    if (scan->end - scan->at < 4) {
                        return 0;
                    }

                    for (unsigned at = 0; at < 4u; ++at) {
                        uint32_t digit;

                        if (!hex(*scan->at++, &digit)) {
                            return 0;
                        }

                        codepoint = codepoint * 16u + digit;
                    }

                    if (!append_codepoint(output, capacity, &size, codepoint)) {
                        return 0;
                    }

                    continue;

                default: return 0;
            }
        }

        if (!append_byte(output, capacity, &size, value)) {
            return 0;
        }
    }

    if (scan->at == scan->end) {
        return 0;
    }

    ++scan->at;
    output[size] = '\0';

    return 1;
}

/** Reads a whole number of digits only, no sign; 0 when there is none or it passes 32 bits. */
static int unsigned_value(json_scan *scan, uint32_t *result) {
    uint64_t value  = 0;
    int      digits = 0;

    space(scan);

    while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') {
        value = value * 10u + (unsigned)(*scan->at++ - '0');

        if (value > UINT32_MAX) {
            return 0;
        }

        digits = 1;
    }

    if (!digits) {
        return 0;
    }

    *result = (uint32_t)value;

    return 1;
}

/**
 * `ConversionQuality` as the protocol sends it: `1`, `0`, or up to three decimals, in thousandths.
 * Returns 0 when the number is malformed or above 1.
 */
static int quality_value(json_scan *scan, uint32_t *result) {
    uint32_t whole    = 0;
    uint32_t fraction = 0;
    unsigned digits   = 0;
    int      seen     = 0;

    /* The whole part: 0 or 1. */
    space(scan);

    while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') {
        whole = whole * 10u + (uint32_t)(*scan->at++ - '0');

        if (whole > 1u) {
            return 0;
        }

        seen = 1;
    }

    if (!seen) {
        return 0;
    }

    /* The decimals, when there is a point: at least one digit, at most three. */
    if (scan->at < scan->end && *scan->at == '.') {
        ++scan->at;

        if (scan->at == scan->end || *scan->at < '0' || *scan->at > '9') {
            return 0;
        }

        while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') {
            if (digits >= 3u) {
                return 0;
            }

            fraction = fraction * 10u + (uint32_t)(*scan->at++ - '0');
            ++digits;
        }
    }

    /* Fewer than three decimals are made up to thousandths: 0.5 becomes 500. */
    while (digits < 3u) {
        fraction *= 10u;
        ++digits;
    }

    *result = whole * EDDS_QUALITY_SCALE + fraction;

    return *result <= EDDS_QUALITY_SCALE;
}

/** Steps over one JSON value of any kind; written out below. */
static int skip_value(json_scan *scan, unsigned depth);

/** Steps over one JSON string without keeping it; 0 when it is malformed. */
static int skip_string(json_scan *scan) {
    char        scratch[2];
    const char *start = scan->at;

    /* A string of at most one byte is read whole; a longer one is checked byte by byte below. */
    if (string_value(scan, scratch, sizeof scratch)) {
        return 1;
    }

    scan->at = start;

    if (!take(scan, '"')) {
        return 0;
    }

    while (scan->at < scan->end) {
        unsigned char value = (unsigned char)*scan->at++;

        if (value == '"') {
            return 1;
        }

        if (value < 0x20u) {
            return 0;
        }

        /* An escape: `u` and four hexadecimal digits, or a quote, backslash, slash or b f n r t. */
        if (value == '\\') {
            if (scan->at == scan->end) {
                return 0;
            }

            value = (unsigned char)*scan->at++;

            if (value == 'u') {
                if (scan->end - scan->at < 4) {
                    return 0;
                }

                for (unsigned at = 0; at < 4u; ++at) {
                    uint32_t digit;

                    if (!hex(*scan->at++, &digit)) {
                        return 0;
                    }
                }
            } else if (strchr("\"\\/bfnrt", value) == NULL) {
                return 0;
            }
        }
    }

    return 0;
}

/**
 * Steps over one JSON value of any kind: a string, an object, an array, true, false, null or a
 * number. A value more than 32 levels down is refused. Returns 0 when the value is malformed.
 */
static int skip_value(json_scan *scan, unsigned depth) {
    if (depth > 32u) {
        return 0;
    }

    space(scan);

    if (scan->at == scan->end) {
        return 0;
    }

    if (*scan->at == '"') {
        return skip_string(scan);
    }

    /* An object: string keys, each with a colon and a value, separated by commas. */
    if (*scan->at == '{') {
        ++scan->at;
        space(scan);

        if (take(scan, '}')) {
            return 1;
        }

        for (;;) {
            if (!skip_string(scan) || !take(scan, ':') || !skip_value(scan, depth + 1u)) {
                return 0;
            }

            if (take(scan, '}')) {
                return 1;
            }

            if (!take(scan, ',')) {
                return 0;
            }
        }
    }

    /* An array: values separated by commas. */
    if (*scan->at == '[') {
        ++scan->at;
        space(scan);

        if (take(scan, ']')) {
            return 1;
        }

        for (;;) {
            if (!skip_value(scan, depth + 1u)) {
                return 0;
            }

            if (take(scan, ']')) {
                return 1;
            }

            if (!take(scan, ',')) {
                return 0;
            }
        }
    }

    if (literal(scan, "true") || literal(scan, "false") || literal(scan, "null")) {
        return 1;
    }

    /* Anything else must be a number: a sign, digits, decimals, an exponent, each optional. */
    {
        const char *start = scan->at;

        if (*scan->at == '-') {
            ++scan->at;
        }

        while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') {
            ++scan->at;
        }

        if (scan->at < scan->end && *scan->at == '.') {
            ++scan->at;

            while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') {
                ++scan->at;
            }
        }

        if (scan->at < scan->end && (*scan->at == 'e' || *scan->at == 'E')) {
            ++scan->at;

            if (scan->at < scan->end && (*scan->at == '+' || *scan->at == '-')) {
                ++scan->at;
            }

            while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') {
                ++scan->at;
            }
        }

        return scan->at > start;
    }
}

/**
 * A first look at a record: its "protocolVersion" and its "kind", every other field skipped.
 * Returns 1 when the line is one JSON object that has both, each once.
 */
static int discover(const char *line, size_t size, uint32_t *version, char kind[32]) {
    json_scan scan         = { line, line + size };
    int       have_version = 0;
    int       have_kind    = 0;

    if (!take(&scan, '{')) {
        return 0;
    }

    if (take(&scan, '}')) {
        return 0;
    }

    /* Field by field: a key, a colon, a value, then a comma or the closing brace. */
    for (;;) {
        char key[64];

        if (!string_value(&scan, key, sizeof key) || !take(&scan, ':')) {
            return 0;
        }

        if (strcmp(key, "protocolVersion") == 0) {
            if (have_version || !unsigned_value(&scan, version)) {
                return 0;
            }

            have_version = 1;
        } else if (strcmp(key, "kind") == 0) {
            if (have_kind || !string_value(&scan, kind, 32u)) {
                return 0;
            }

            have_kind = 1;
        } else if (!skip_value(&scan, 0u)) {
            return 0;
        }

        if (take(&scan, '}')) {
            break;
        }

        if (!take(&scan, ',')) {
            return 0;
        }
    }

    /* Nothing but whitespace may follow the object. */
    space(&scan);

    return scan.at == scan.end && have_version && have_kind;
}

/** Reads true or false into `*result` as 1 or 0; 0 when it is neither. */
static int bool_value(json_scan *scan, int *result) {
    if (literal(scan, "true")) {
        *result = 1;
        return 1;
    }

    if (literal(scan, "false")) {
        *result = 0;
        return 1;
    }

    return 0;
}

/**
 * Reads a job's "profile": an object with all thirteen Workbench settings, each exactly once.
 * Returns 0 when it is malformed. A profile that reads well but that edds_profile_check refuses
 * also returns 0, with the refusal in `*profile_status` and `*profile_error`.
 */
static int profile_value(
    json_scan    *scan,
    edds_profile *profile,
    edds_status  *profile_status,
    edds_error   *profile_error) {
    unsigned fields = 0;

    if (!take(scan, '{')) {
        return 0;
    }

    edds_default_profile(profile);

    /* Setting by setting: each has its own bit in `fields`, and its value is checked as read. */
    for (;;) {
        char     key[64];
        char     value[64];
        uint32_t number;
        int      boolean;
        unsigned bit;

        if (!string_value(scan, key, sizeof key) || !take(scan, ':')) {
            return 0;
        }

        if (strcmp(key, "TargetFormat") == 0) {
            bit = 1u << 0;

            if (!string_value(scan, value, sizeof value) || strcmp(value, "EnfusionDDS") != 0) {
                return 0;
            }
        } else if (strcmp(key, "FormatCompress") == 0) {
            bit = 1u << 1;

            if (!string_value(scan, value, sizeof value)) {
                return 0;
            }

            if (strcmp(value, "Copy") == 0) {
                profile->format_compress = EDDS_COMPRESS_COPY;
            } else if (strcmp(value, "Fastest") == 0) {
                profile->format_compress = EDDS_COMPRESS_FASTEST;
            } else if (strcmp(value, "Medium") == 0) {
                profile->format_compress = EDDS_COMPRESS_MEDIUM;
            } else if (strcmp(value, "Best") == 0) {
                profile->format_compress = EDDS_COMPRESS_BEST;
            } else {
                return 0;
            }
        } else if (strcmp(key, "CompressTreshold") == 0) {
            bit = 1u << 2;

            if (!unsigned_value(scan, &number) || number > 100u) {
                return 0;
            }

            profile->compress_threshold = number;
        } else if (strcmp(key, "RemoveMips") == 0) {
            bit = 1u << 3;

            if (!unsigned_value(scan, &number)) {
                return 0;
            }

            profile->remove_mips = number;
        } else if (strcmp(key, "Conversion") == 0) {
            /* A Workbench name of a conversion this converter supports. */
            const edds_conversion_capability *capability;

            bit = 1u << 4;

            if (!string_value(scan, value, sizeof value)) {
                return 0;
            }

            capability = edds_conversion_of_workbench_name(value);

            if (capability == NULL || !capability->supported) {
                return 0;
            }

            profile->conversion = capability->conversion;
        } else if (strcmp(key, "ConversionQuality") == 0) {
            bit = 1u << 5;

            if (!quality_value(scan, &profile->conversion_quality)) {
                return 0;
            }
        } else if (strcmp(key, "Swizzling") == 0) {
            /* An unknown name becomes EDDS_SWIZZLE_UNKNOWN, which edds_profile_check refuses. */
            const edds_swizzle_capability *capability;

            bit = 1u << 6;

            if (!string_value(scan, value, sizeof value)) {
                return 0;
            }

            capability         = edds_swizzle_of_workbench_name(value);
            profile->swizzling = capability == NULL ? EDDS_SWIZZLE_UNKNOWN : capability->swizzling;
        } else if (strcmp(key, "ContainsMips") == 0) {
            bit = 1u << 7;

            if (!bool_value(scan, &boolean)) {
                return 0;
            }

            profile->contains_mips = boolean;
        } else if (strcmp(key, "GenerateMips") == 0) {
            bit = 1u << 8;

            if (!bool_value(scan, &boolean)) {
                return 0;
            }

            profile->generate_mips = boolean;
        } else if (strcmp(key, "Normalize") == 0) {
            bit = 1u << 9;

            if (!bool_value(scan, &boolean)) {
                return 0;
            }

            profile->normalize = boolean;
        } else if (strcmp(key, "MipMapFunction") == 0) {
            bit = 1u << 10;

            if (!string_value(scan, value, sizeof value)) {
                return 0;
            }

            if (strcmp(value, "Filter") == 0) {
                profile->mipmap_function = EDDS_MIPMAP_FILTER;
            } else if (strcmp(value, "Normalize") == 0) {
                profile->mipmap_function = EDDS_MIPMAP_NORMALIZE;
            } else if (strcmp(value, "ColorNoise") == 0) {
                profile->mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
            } else {
                return 0;
            }
        } else if (strcmp(key, "MipMapFilter") == 0) {
            bit = 1u << 11;

            if (!string_value(scan, value, sizeof value)) {
                return 0;
            }

            if (strcmp(value, "Box") == 0) {
                profile->mipmap_filter = EDDS_FILTER_BOX;
            } else if (strcmp(value, "Kaiser") == 0) {
                profile->mipmap_filter = EDDS_FILTER_KAISER;
            } else if (strcmp(value, "Triangle") == 0) {
                profile->mipmap_filter = EDDS_FILTER_TRIANGLE;
            } else {
                return 0;
            }
        } else if (strcmp(key, "TiledTexture") == 0) {
            bit = 1u << 12;

            if (!bool_value(scan, &boolean)) {
                return 0;
            }

            profile->tiled_texture = boolean;
        } else {
            return 0;
        }

        /* A setting given twice is refused. */
        if ((fields & bit) != 0u) {
            return 0;
        }

        fields |= bit;

        if (take(scan, '}')) {
            break;
        }

        if (!take(scan, ',')) {
            return 0;
        }
    }

    /* All thirteen bits: every setting is there. */
    if (fields != 0x1fffu) {
        return 0;
    }

    {
        /* A quality against the conversion it would reach is only decidable once both are read. */
        *profile_status = edds_profile_check(profile, profile_error);
        return *profile_status == EDDS_OK;
    }
}

/**
 * Reads a job's "identity": null, or an object with the strings "guid", "name" and "sourceFile",
 * each exactly once. Returns 0 when it is malformed.
 */
static int identity_value(json_scan *scan, edds_batch_job *job) {
    unsigned fields = 0;

    if (literal(scan, "null")) {
        return 1;
    }

    if (!take(scan, '{')) {
        return 0;
    }

    /* Field by field, each with its own bit in `fields`. */
    for (;;) {
        char     key[64];
        unsigned bit;

        if (!string_value(scan, key, sizeof key) || !take(scan, ':')) {
            return 0;
        }

        if (strcmp(key, "guid") == 0) {
            bit = 1u;

            if (!string_value(scan, job->guid, sizeof job->guid)) {
                return 0;
            }
        } else if (strcmp(key, "name") == 0) {
            bit = 2u;

            if (!string_value(scan, job->resource_name, sizeof job->resource_name)) {
                return 0;
            }
        } else if (strcmp(key, "sourceFile") == 0) {
            bit = 4u;

            if (!string_value(scan, job->source_file, sizeof job->source_file)) {
                return 0;
            }
        } else {
            return 0;
        }

        if ((fields & bit) != 0u) {
            return 0;
        }

        fields |= bit;

        if (take(scan, '}')) {
            break;
        }

        if (!take(scan, ',')) {
            return 0;
        }
    }

    return fields == 7u;
}

/**
 * Reads a job's "expected": null, or an object with the strings "source", "output" and "metadata",
 * each exactly once, which also sets `has_expected`. Returns 0 when it is malformed.
 */
static int expected_value(json_scan *scan, edds_batch_job *job) {
    unsigned fields = 0;

    if (literal(scan, "null")) {
        return 1;
    }

    if (!take(scan, '{')) {
        return 0;
    }

    job->has_expected = 1;

    /* Field by field: the key picks the bit in `fields` and the string the value goes into. */
    for (;;) {
        char     key[64];
        char    *output;
        unsigned bit;

        if (!string_value(scan, key, sizeof key) || !take(scan, ':')) {
            return 0;
        }

        if (strcmp(key, "source") == 0) {
            bit    = 1u;
            output = job->expected_source;
        } else if (strcmp(key, "output") == 0) {
            bit    = 2u;
            output = job->expected_output;
        } else if (strcmp(key, "metadata") == 0) {
            bit    = 4u;
            output = job->expected_metadata;
        } else {
            return 0;
        }

        if ((fields & bit) != 0u || !string_value(scan, output, 64u)) {
            return 0;
        }

        fields |= bit;

        if (take(scan, '}')) {
            break;
        }

        if (!take(scan, ',')) {
            return 0;
        }
    }

    return fields == 7u;
}

/**
 * Reads a header or an end record in full: "protocolVersion" 1, "kind" `expected_kind`, and for a
 * header (`count` not NULL) its "jobCount" into `*count`; each once and nothing else. Returns 1
 * when the record is exactly that.
 */
static int simple_record(const char *line, size_t size, const char *expected_kind,
    uint32_t *count) {
    json_scan scan   = { line, line + size };
    unsigned  fields = 0;

    if (!take(&scan, '{')) {
        return 0;
    }

    /* Field by field, each with its own bit in `fields`. */
    for (;;) {
        char     key[64];
        char     value[32];
        uint32_t number;
        unsigned bit;

        if (!string_value(&scan, key, sizeof key) || !take(&scan, ':')) {
            return 0;
        }

        if (strcmp(key, "protocolVersion") == 0) {
            bit = 1u;

            if (!unsigned_value(&scan, &number) || number != 1u) {
                return 0;
            }
        } else if (strcmp(key, "kind") == 0) {
            bit = 2u;

            if (!string_value(&scan, value, sizeof value) || strcmp(value, expected_kind) != 0) {
                return 0;
            }
        } else if (count != NULL && strcmp(key, "jobCount") == 0) {
            bit = 4u;

            if (!unsigned_value(&scan, count)) {
                return 0;
            }
        } else {
            return 0;
        }

        if ((fields & bit) != 0u) {
            return 0;
        }

        fields |= bit;

        if (take(&scan, '}')) {
            break;
        }

        if (!take(&scan, ',')) {
            return 0;
        }
    }

    /* Nothing may follow the object, and every field is there: two, or three for a header. */
    space(&scan);

    return scan.at == scan.end && fields == (count == NULL ? 3u : 7u);
}

/**
 * Reads a job record in full: every field exactly once and nothing else, with a non-empty id,
 * input and output. With a metadata path, the identity's guid, name and sourceFile must all be
 * given; without one, they must all be empty. Returns 1 for a well-formed job. A profile that
 * edds_profile_check refuses returns 0 with that refusal in `*profile_status`.
 */
static int job_record(
    const char     *line,
    size_t          size,
    edds_batch_job *job,
    edds_status    *profile_status,
    edds_error     *profile_error) {
    json_scan scan   = { line, line + size };
    unsigned  fields = 0;

    memset(job, 0, sizeof *job);

    if (!take(&scan, '{')) {
        return 0;
    }

    /* Field by field, each with its own bit in `fields`. */
    for (;;) {
        char     key[64];
        char     value[32];
        uint32_t number;
        unsigned bit;

        if (!string_value(&scan, key, sizeof key) || !take(&scan, ':')) {
            return 0;
        }

        if (strcmp(key, "protocolVersion") == 0) {
            bit = 1u << 0;

            if (!unsigned_value(&scan, &number) || number != 1u) {
                return 0;
            }
        } else if (strcmp(key, "kind") == 0) {
            bit = 1u << 1;

            if (!string_value(&scan, value, sizeof value) || strcmp(value, "job") != 0) {
                return 0;
            }
        } else if (strcmp(key, "id") == 0) {
            bit = 1u << 2;

            if (!string_value(&scan, job->id, sizeof job->id) || job->id[0] == '\0') {
                return 0;
            }
        } else if (strcmp(key, "input") == 0) {
            bit = 1u << 3;

            if (!string_value(&scan, job->input, sizeof job->input) || job->input[0] == '\0') {
                return 0;
            }
        } else if (strcmp(key, "output") == 0) {
            bit = 1u << 4;

            if (!string_value(&scan, job->output, sizeof job->output) || job->output[0] == '\0') {
                return 0;
            }
        } else if (strcmp(key, "metadata") == 0) {
            /* null, or the path of the metadata file to write. */
            bit = 1u << 5;

            if (literal(&scan, "null")) {
                job->has_metadata = 0;
            } else {
                job->has_metadata = 1;

                if (!string_value(&scan, job->metadata, sizeof job->metadata)) {
                    return 0;
                }
            }
        } else if (strcmp(key, "identity") == 0) {
            bit = 1u << 6;

            if (!identity_value(&scan, job)) {
                return 0;
            }
        } else if (strcmp(key, "profile") == 0) {
            bit = 1u << 7;

            if (!profile_value(&scan, &job->profile, profile_status, profile_error)) {
                return 0;
            }
        } else if (strcmp(key, "expected") == 0) {
            bit = 1u << 8;

            if (!expected_value(&scan, job)) {
                return 0;
            }
        } else {
            return 0;
        }

        if ((fields & bit) != 0u) {
            return 0;
        }

        fields |= bit;

        if (take(&scan, '}')) {
            break;
        }

        if (!take(&scan, ',')) {
            return 0;
        }
    }

    /* Nothing may follow the object, and all nine fields are there. */
    space(&scan);

    if (scan.at != scan.end || fields != 0x1ffu) {
        return 0;
    }

    /* The identity goes with the metadata: all of it with a metadata path, none of it without. */
    return job->has_metadata
        ? job->metadata[0] != '\0' &&
            job->guid[0] != '\0' &&
            job->resource_name[0] != '\0' &&
            job->source_file[0] != '\0'
        : job->guid[0] == '\0' && job->resource_name[0] == '\0' && job->source_file[0] == '\0';
}

/**
 * Two readings of the line: discover finds its protocol version and kind, then the reader of that
 * kind reads it again in full and refuses anything it does not know.
 */
edds_status edds_batch_parse_line(
    const char        *line,
    size_t             size,
    edds_batch_record *record,
    edds_error        *error) {
    uint32_t    version        = 0;
    char        kind[32]       = { 0 };
    edds_status profile_status = EDDS_OK;

    if (line == NULL ||
        record == NULL ||
        error == NULL ||
        size == 0u ||
        size > EDDS_BATCH_MAX_LINE_BYTES) {
        return fail(error, "batch-line-size",
            "A batch NDJSON record is empty or exceeds the hard line limit.");
    }

    if (!discover(line, size, &version, kind)) {
        return fail(error, "malformed-batch-record",
            "A batch input line is not a valid NDJSON object.");
    }

    if (version != 1u) {
        return fail(error, "incompatible-batch-version",
            "A batch input record uses an incompatible protocol version.");
    }

    memset(record, 0, sizeof *record);

    if (strcmp(kind, "batch") == 0) {
        record->kind = EDDS_BATCH_HEADER;

        if (!simple_record(line, size, "batch", &record->job_count)) {
            goto malformed;
        }

        return EDDS_OK;
    }

    if (strcmp(kind, "job") == 0) {
        record->kind = EDDS_BATCH_JOB;

        if (!job_record(line, size, &record->job, &profile_status, error)) {
            /* A profile refused by edds_profile_check keeps that refusal. */
            if (profile_status != EDDS_OK) {
                return profile_status;
            }

            goto malformed;
        }

        return EDDS_OK;
    }

    if (strcmp(kind, "end") == 0) {
        record->kind = EDDS_BATCH_END;

        if (!simple_record(line, size, "end", NULL)) {
            goto malformed;
        }

        return EDDS_OK;
    }

    return fail(error, "unknown-batch-record", "A batch input record has an unknown kind.");

malformed:
    return fail(error, "malformed-batch-record",
        "A batch input record is incomplete, duplicated, or unsupported.");
}

/** Clears the reader: no line gathered, nothing being swallowed. */
void edds_batch_reader_init(edds_batch_reader *reader) {
    if (reader != NULL) {
        memset(reader, 0, sizeof *reader);
    }
}

/**
 * Ends the line gathered so far. A line that went over the limit is reported as OVERFLOW;
 * otherwise READY, the line NUL-terminated in `reader->line` and its length in `*size`.
 */
static edds_batch_line finish_line(edds_batch_reader *reader, size_t *size) {
    if (reader->overflowed) {
        reader->overflowed = 0;
        reader->size       = 0;

        *size = 0;
        return EDDS_BATCH_LINE_OVERFLOW;
    }

    /* A line that ended in CR LF loses the CR too. */
    if (reader->size > 0u && reader->line[reader->size - 1u] == '\r') {
        --reader->size;
    }

    reader->line[reader->size] = '\0';

    /* The caller gets the line's length; the next byte starts a new line. */
    *size        = reader->size;
    reader->size = 0;

    return EDDS_BATCH_LINE_READY;
}

/** Gathers bytes into the line until a newline ends it; see batch.h for what each result means. */
edds_batch_line edds_batch_reader_push(
    edds_batch_reader *reader,
    const char        *data,
    size_t             data_size,
    size_t            *consumed,
    size_t            *size) {
    size_t at = 0;

    /* Missing arguments: nothing is taken and no line is reported. */
    if (reader == NULL || consumed == NULL || size == NULL || (data == NULL && data_size != 0u)) {
        if (consumed != NULL) {
            *consumed = 0;
        }

        if (size != NULL) {
            *size = 0;
        }

        return EDDS_BATCH_LINE_PENDING;
    }

    *size = 0;

    while (at < data_size) {
        const char value = data[at++];

        /* A newline ends the line; the bytes behind it stay for the next push. */
        if (value == '\n') {
            *consumed = at;
            return finish_line(reader, size);
        }

        if (reader->size >= EDDS_BATCH_MAX_LINE_BYTES) {
            /* The rest of an oversized line is swallowed, never framed into half a record. */
            reader->overflowed = 1;
            reader->size       = 0;
            continue;
        }

        if (!reader->overflowed) {
            reader->line[reader->size++] = value;
        }
    }

    *consumed = at;

    return EDDS_BATCH_LINE_PENDING;
}

/** At the end of the stream: the line still gathered, if any, ended as a newline would end it. */
edds_batch_line edds_batch_reader_finish(edds_batch_reader *reader, size_t *size) {
    if (reader == NULL || size == NULL) {
        return EDDS_BATCH_LINE_PENDING;
    }

    *size = 0;

    if (reader->overflowed) {
        reader->overflowed = 0;
        reader->size       = 0;
        return EDDS_BATCH_LINE_OVERFLOW;
    }

    if (reader->size == 0u) {
        return EDDS_BATCH_LINE_PENDING;
    }

    return finish_line(reader, size);
}
