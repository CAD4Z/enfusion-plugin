#include <edds/edds.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum token_kind {
    TOKEN_END,
    TOKEN_WORD,
    TOKEN_STRING,
    TOKEN_OPEN,
    TOKEN_CLOSE,
    TOKEN_COLON,
    TOKEN_INVALID
} token_kind;

typedef struct token {
    token_kind kind;
    char text[EDDS_METADATA_PATH_BYTES];
} token;

typedef struct scanner {
    const char *source;
    size_t size;
    size_t at;
} scanner;

enum setting_bit {
    SETTING_SOURCE = 1u << 0,
    SETTING_TARGET = 1u << 1,
    SETTING_FORMAT = 1u << 2,
    SETTING_THRESHOLD = 1u << 3,
    SETTING_REMOVE_MIPS = 1u << 4,
    SETTING_CONVERSION = 1u << 5,
    SETTING_QUALITY = 1u << 6,
    SETTING_SWIZZLING = 1u << 7,
    SETTING_CONTAINS_MIPS = 1u << 8,
    SETTING_GENERATE_MIPS = 1u << 9,
    SETTING_NORMALIZE = 1u << 10,
    SETTING_MIP_FUNCTION = 1u << 11,
    SETTING_MIP_FILTER = 1u << 12,
    SETTING_TILED = 1u << 13,
    SETTING_CUBEMAP = 1u << 14
};

static void fail(edds_error *error, const char *code, const char *format, ...) {
    va_list arguments;
    if (error == NULL) return;
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof error->message, format, arguments);
    va_end(arguments);
}

static int read_text(FILE *input, char **source, size_t *size, edds_error *error) {
    long length;
    char *allocation;
    *source = NULL;
    *size = 0;
    if (fseek(input, 0, SEEK_END) != 0 || (length = ftell(input)) < 0 ||
        fseek(input, 0, SEEK_SET) != 0 || (uint64_t)length > EDDS_MAX_FILE_BYTES) {
        fail(error, "metadata-size-limit", "The metadata could not be measured within the input limit.");
        return 0;
    }
    allocation = malloc((size_t)length + 1u);
    if (allocation == NULL) {
        fail(error, "allocation-failed", "Memory for the metadata could not be allocated.");
        return 0;
    }
    if (fread(allocation, 1, (size_t)length, input) != (size_t)length) {
        free(allocation);
        fail(error, "metadata-read-failed", "The metadata could not be read completely.");
        return 0;
    }
    allocation[length] = '\0';
    *source = allocation;
    *size = (size_t)length;
    return 1;
}

static int skip_space(scanner *scan) {
    for (;;) {
        while (scan->at < scan->size && isspace((unsigned char)scan->source[scan->at])) ++scan->at;
        if (scan->at + 1u >= scan->size || scan->source[scan->at] != '/') return 1;
        if (scan->source[scan->at + 1u] == '/') {
            scan->at += 2;
            while (scan->at < scan->size && scan->source[scan->at] != '\n') ++scan->at;
        } else if (scan->source[scan->at + 1u] == '*') {
            scan->at += 2;
            while (scan->at + 1u < scan->size &&
                   !(scan->source[scan->at] == '*' && scan->source[scan->at + 1u] == '/')) {
                ++scan->at;
            }
            if (scan->at + 1u >= scan->size) return 0;
            scan->at += 2;
        } else {
            return 1;
        }
    }
}

static token next_token(scanner *scan) {
    token result;
    size_t length = 0;
    memset(&result, 0, sizeof result);
    if (!skip_space(scan)) {
        result.kind = TOKEN_INVALID;
        return result;
    }
    if (scan->at == scan->size) {
        result.kind = TOKEN_END;
        return result;
    }
    if (scan->source[scan->at] == '{') {
        ++scan->at;
        result.kind = TOKEN_OPEN;
        return result;
    }
    if (scan->source[scan->at] == '}') {
        ++scan->at;
        result.kind = TOKEN_CLOSE;
        return result;
    }
    if (scan->source[scan->at] == ':') {
        ++scan->at;
        result.kind = TOKEN_COLON;
        return result;
    }
    if (scan->source[scan->at] == '"') {
        ++scan->at;
        result.kind = TOKEN_STRING;
        while (scan->at < scan->size && scan->source[scan->at] != '"') {
            char value = scan->source[scan->at++];
            if (value == '\\' && scan->at < scan->size &&
                (scan->source[scan->at] == '\\' || scan->source[scan->at] == '"')) {
                value = scan->source[scan->at++];
            }
            if (length + 1u >= sizeof result.text || (unsigned char)value < 0x20u) {
                result.kind = TOKEN_INVALID;
                return result;
            }
            result.text[length++] = value;
        }
        if (scan->at >= scan->size) {
            result.kind = TOKEN_INVALID;
            return result;
        }
        ++scan->at;
        result.text[length] = '\0';
        return result;
    }
    result.kind = TOKEN_WORD;
    while (scan->at < scan->size && !isspace((unsigned char)scan->source[scan->at]) &&
           scan->source[scan->at] != '{' && scan->source[scan->at] != '}' &&
           scan->source[scan->at] != ':' && scan->source[scan->at] != '"') {
        if (length + 1u >= sizeof result.text) {
            result.kind = TOKEN_INVALID;
            return result;
        }
        result.text[length++] = scan->source[scan->at++];
    }
    if (length == 0) {
        result.kind = TOKEN_INVALID;
        return result;
    }
    result.text[length] = '\0';
    return result;
}

static int skip_open_block(scanner *scan) {
    unsigned depth = 1;
    while (depth != 0) {
        const token value = next_token(scan);
        if (value.kind == TOKEN_OPEN) ++depth;
        else if (value.kind == TOKEN_CLOSE) --depth;
        else if (value.kind == TOKEN_END || value.kind == TOKEN_INVALID) return 0;
    }
    return 1;
}

static int copy_text(char *destination, size_t capacity, const char *source) {
    const size_t size = strlen(source);
    if (size >= capacity) return 0;
    memcpy(destination, source, size + 1u);
    return 1;
}

static int unsigned_value(const token *value, uint32_t *result) {
    uint64_t parsed = 0;
    if (value->kind != TOKEN_WORD || value->text[0] == '\0') return 0;
    for (const char *at = value->text; *at != '\0'; ++at) {
        if (*at < '0' || *at > '9') return 0;
        parsed = parsed * 10u + (unsigned)(*at - '0');
        if (parsed > UINT32_MAX) return 0;
    }
    *result = (uint32_t)parsed;
    return 1;
}

/**
 * `ConversionQuality` as thousandths. Workbench writes it as a plain `1` or as up to three
 * decimals, and those are exactly the values that survive a round trip through this converter;
 * a fourth decimal would have to be rounded on the way back out and is refused instead.
 */
static int quality_value(const token *value, uint32_t *result) {
    const char *at = value->text;
    uint32_t whole = 0;
    uint32_t fraction = 0;
    unsigned digits = 0;
    if (value->kind != TOKEN_WORD || *at < '0' || *at > '9') return 0;
    while (*at >= '0' && *at <= '9') {
        whole = whole * 10u + (uint32_t)(*at - '0');
        if (whole > 1u) return 0;
        ++at;
    }
    if (*at == '.') {
        ++at;
        if (*at < '0' || *at > '9') return 0;
        while (*at >= '0' && *at <= '9') {
            if (digits >= 3u) return 0;
            fraction = fraction * 10u + (uint32_t)(*at - '0');
            ++digits;
            ++at;
        }
    }
    if (*at != '\0') return 0;
    while (digits < 3u) {
        fraction *= 10u;
        ++digits;
    }
    *result = whole * EDDS_QUALITY_SCALE + fraction;
    return *result <= EDDS_QUALITY_SCALE;
}

/** The shortest text that reads back as exactly this quality: `1`, `0.5`, `0.403`. */
static void quality_text(uint32_t value, char text[8]) {
    const uint32_t whole = value / EDDS_QUALITY_SCALE;
    const uint32_t fraction = value % EDDS_QUALITY_SCALE;
    if (fraction == 0) {
        (void)snprintf(text, 8, "%u", whole);
    } else if (fraction % 100u == 0) {
        (void)snprintf(text, 8, "%u.%u", whole, fraction / 100u);
    } else if (fraction % 10u == 0) {
        (void)snprintf(text, 8, "%u.%02u", whole, fraction / 10u);
    } else {
        (void)snprintf(text, 8, "%u.%03u", whole, fraction);
    }
}

static int is_one_of(const char *value, const char *const *choices, size_t count) {
    for (size_t at = 0; at < count; ++at) {
        if (strcmp(value, choices[at]) == 0) return 1;
    }
    return 0;
}

static edds_status unsupported(edds_error *error, const char *key, const char *value) {
    fail(error, "unsupported-setting", "Workbench setting %s=%s is recognized but unsupported.", key, value);
    return EDDS_UNSUPPORTED_FORMAT;
}

static edds_status recipe_setting(
    edds_metadata *metadata,
    uint32_t *seen,
    const token *key,
    const token *value,
    edds_error *error
) {
    uint32_t bit = 0;
    uint32_t number = 0;
    if (strcmp(key->text, "SourceFile") == 0) bit = SETTING_SOURCE;
    else if (strcmp(key->text, "TargetFormat") == 0) bit = SETTING_TARGET;
    else if (strcmp(key->text, "FormatCompress") == 0) bit = SETTING_FORMAT;
    else if (strcmp(key->text, "CompressTreshold") == 0) bit = SETTING_THRESHOLD;
    else if (strcmp(key->text, "RemoveMips") == 0) bit = SETTING_REMOVE_MIPS;
    else if (strcmp(key->text, "Conversion") == 0) bit = SETTING_CONVERSION;
    else if (strcmp(key->text, "ConversionQuality") == 0) bit = SETTING_QUALITY;
    else if (strcmp(key->text, "Swizzling") == 0) bit = SETTING_SWIZZLING;
    else if (strcmp(key->text, "ContainsMips") == 0) bit = SETTING_CONTAINS_MIPS;
    else if (strcmp(key->text, "GenerateMips") == 0) bit = SETTING_GENERATE_MIPS;
    else if (strcmp(key->text, "Normalize") == 0) bit = SETTING_NORMALIZE;
    else if (strcmp(key->text, "MipMapFunction") == 0) bit = SETTING_MIP_FUNCTION;
    else if (strcmp(key->text, "MipMapFilter") == 0) bit = SETTING_MIP_FILTER;
    else if (strcmp(key->text, "TiledTexture") == 0) bit = SETTING_TILED;
    else if (strcmp(key->text, "GenerateCubemap") == 0) bit = SETTING_CUBEMAP;
    else return EDDS_OK;
    if ((*seen & bit) != 0) {
        fail(error, "duplicate-setting", "Workbench setting %s occurs more than once in PC.", key->text);
        return EDDS_INVALID_INPUT;
    }
    *seen |= bit;
    if (bit == SETTING_SOURCE) {
        if (value->kind != TOKEN_STRING ||
            !copy_text(metadata->source_file, sizeof metadata->source_file, value->text)) goto malformed;
        return EDDS_OK;
    }
    if (value->kind != TOKEN_WORD) goto malformed;
    if (bit == SETTING_TARGET) {
        static const char *const known[] = { "EnfusionDDS", "EnfusionDDS_LZ0", "EnfusionDDS_LZ4", "DirectXDDS" };
        if (!is_one_of(value->text, known, sizeof known / sizeof known[0])) goto malformed;
        return strcmp(value->text, "EnfusionDDS") == 0 ? EDDS_OK : unsupported(error, key->text, value->text);
    }
    if (bit == SETTING_FORMAT) {
        if (strcmp(value->text, "Copy") == 0) metadata->profile.format_compress = EDDS_COMPRESS_COPY;
        else if (strcmp(value->text, "Fastest") == 0) metadata->profile.format_compress = EDDS_COMPRESS_FASTEST;
        else if (strcmp(value->text, "Medium") == 0) metadata->profile.format_compress = EDDS_COMPRESS_MEDIUM;
        else if (strcmp(value->text, "Best") == 0) metadata->profile.format_compress = EDDS_COMPRESS_BEST;
        else goto malformed;
        return EDDS_OK;
    }
    if (bit == SETTING_CONVERSION) {
        const edds_conversion_capability *capability =
            edds_conversion_of_workbench_name(value->text);
        if (capability == NULL) goto malformed;
        metadata->profile.conversion = capability->conversion;
        return capability->supported ? EDDS_OK : unsupported(error, key->text, value->text);
    }
    if (bit == SETTING_QUALITY) {
        if (!quality_value(value, &metadata->profile.conversion_quality)) goto malformed;
        return EDDS_OK;
    }
    if (bit == SETTING_SWIZZLING) {
        static const char *const known[] = {
            "None", "NormalMap", "NormalMapY", "NormalMapAlpha", "NormalMapAlphaY",
            "NormalMapXZY", "NormalMapXZYAlpha", "NormalSpecularMap", "NormalSpecularMapXYZS",
            "AmbientSpecularMapGA"
        };
        if (!is_one_of(value->text, known, sizeof known / sizeof known[0])) goto malformed;
        return strcmp(value->text, "None") == 0 ? EDDS_OK : unsupported(error, key->text, value->text);
    }
    if (bit == SETTING_MIP_FUNCTION) {
        static const char *const known[] = { "Filter", "ColorNoise", "Normalize" };
        if (!is_one_of(value->text, known, sizeof known / sizeof known[0])) goto malformed;
        return strcmp(value->text, "Filter") == 0 ? EDDS_OK : unsupported(error, key->text, value->text);
    }
    if (bit == SETTING_MIP_FILTER) {
        static const char *const known[] = { "Box", "Kaiser", "Triangle" };
        if (!is_one_of(value->text, known, sizeof known / sizeof known[0])) goto malformed;
        return strcmp(value->text, "Box") == 0 ? EDDS_OK : unsupported(error, key->text, value->text);
    }
    if (!unsigned_value(value, &number)) goto malformed;
    if (bit == SETTING_THRESHOLD) {
        if (number > 100u) return unsupported(error, key->text, value->text);
        metadata->profile.compress_threshold = number;
    } else if (bit == SETTING_GENERATE_MIPS) {
        if (number > 1u) goto malformed;
        metadata->profile.generate_mips = number != 0;
    } else if ((bit == SETTING_REMOVE_MIPS || bit == SETTING_CONTAINS_MIPS ||
                bit == SETTING_NORMALIZE || bit == SETTING_CUBEMAP) && number != 0u) {
        return unsupported(error, key->text, value->text);
    } else if (bit == SETTING_TILED && number != 1u) {
        return unsupported(error, key->text, value->text);
    }
    return EDDS_OK;

malformed:
    fail(error, "malformed-setting", "Workbench setting %s has a malformed or unknown value.", key->text);
    return EDDS_INVALID_INPUT;
}

static edds_status parse_recipe(
    scanner *scan,
    edds_metadata *metadata,
    edds_error *error
) {
    uint32_t seen = 0;
    edds_status pending = EDDS_OK;
    edds_error pending_error = { { 0 }, { 0 } };
    for (;;) {
        const token key = next_token(scan);
        token value;
        edds_status status;
        if (key.kind == TOKEN_CLOSE) break;
        if (key.kind != TOKEN_WORD) {
            fail(error, "malformed-metadata", "The PC recipe is malformed.");
            return EDDS_INVALID_INPUT;
        }
        value = next_token(scan);
        if (value.kind == TOKEN_OPEN) {
            if (!skip_open_block(scan)) {
                fail(error, "malformed-metadata", "A metadata block is not closed.");
                return EDDS_INVALID_INPUT;
            }
            continue;
        }
        status = recipe_setting(metadata, &seen, &key, &value, error);
        if (status == EDDS_UNSUPPORTED_FORMAT) {
            if (pending == EDDS_OK) pending_error = *error;
            pending = EDDS_UNSUPPORTED_FORMAT;
        } else if (status != EDDS_OK) {
            return status;
        }
    }
    if ((seen & SETTING_SOURCE) == 0 || metadata->source_file[0] == '\0') {
        fail(error, "missing-source-file", "PC metadata must declare a non-empty SourceFile.");
        return EDDS_INVALID_INPUT;
    }
    /*
     * Settings arrive in whatever order the file wrote them, so a combination of two of them —
     * a quality against the conversion it would reach — is only decidable once the recipe is whole.
     */
    {
        edds_error combination;
        const edds_status status = edds_profile_check(&metadata->profile, &combination);
        if (status != EDDS_OK && pending == EDDS_OK) {
            pending = status;
            pending_error = combination;
        }
    }
    if (pending != EDDS_OK) *error = pending_error;
    return pending;
}

static edds_status parse_configurations(
    scanner *scan,
    edds_metadata *metadata,
    int *found_pc,
    edds_error *error
) {
    edds_status pending = EDDS_OK;
    edds_error pending_error = { { 0 }, { 0 } };
    for (;;) {
        token resource = next_token(scan);
        token platform;
        token next;
        const edds_source_capability *recognized;
        if (resource.kind == TOKEN_CLOSE) {
            if (pending != EDDS_OK) *error = pending_error;
            return pending;
        }
        if (resource.kind != TOKEN_WORD) goto malformed;
        platform = next_token(scan);
        if (platform.kind != TOKEN_WORD) goto malformed;
        next = next_token(scan);
        if (next.kind == TOKEN_COLON) {
            if (next_token(scan).kind != TOKEN_WORD) goto malformed;
            next = next_token(scan);
        }
        if (next.kind != TOKEN_OPEN) goto malformed;
        recognized = edds_source_capability_of_resource_class(resource.text);
        if (recognized != NULL && strcmp(platform.text, "PC") == 0) {
            edds_status status;
            if (*found_pc) {
                fail(error, "duplicate-pc-recipe",
                    "Metadata contains more than one source-image PC recipe.");
                return EDDS_INVALID_INPUT;
            }
            *found_pc = 1;
            metadata->source_format = recognized->format;
            status = parse_recipe(scan, metadata, error);
            if (status == EDDS_UNSUPPORTED_FORMAT) {
                if (pending == EDDS_OK) pending_error = *error;
                pending = EDDS_UNSUPPORTED_FORMAT;
            } else if (status != EDDS_OK) {
                return status;
            }
        } else if (!skip_open_block(scan)) {
            goto malformed;
        }
    }

malformed:
    fail(error, "malformed-metadata", "The Configurations block is malformed.");
    return EDDS_INVALID_INPUT;
}

static int parse_name(const char *value, edds_metadata *metadata) {
    const char *close;
    if (value[0] != '{' || (close = strchr(value, '}')) == NULL || close - value != 17) return 0;
    for (const char *at = value + 1; at < close; ++at) {
        if (!isxdigit((unsigned char)*at)) return 0;
    }
    if (close[1] == '\0') return 0;
    memcpy(metadata->guid, value + 1, 16);
    metadata->guid[16] = '\0';
    return copy_text(metadata->name, sizeof metadata->name, close + 1);
}

static int extension_is(const char *path, const char *extension) {
    const size_t path_size = strlen(path);
    const size_t extension_size = strlen(extension);
    if (path_size < extension_size) return 0;
    for (size_t at = 0; at < extension_size; ++at) {
        const unsigned char value = (unsigned char)path[path_size - extension_size + at];
        if (tolower(value) != (unsigned char)extension[at]) return 0;
    }
    return 1;
}

edds_status edds_metadata_parse(FILE *input, edds_metadata *metadata, edds_error *error) {
    char *source = NULL;
    size_t size = 0;
    scanner scan;
    token value;
    int found_name = 0;
    int found_configurations = 0;
    int found_pc = 0;
    edds_status status = EDDS_INVALID_INPUT;
    edds_status pending = EDDS_OK;
    edds_error pending_error = { { 0 }, { 0 } };
    if (input == NULL || metadata == NULL) {
        fail(error, "invalid-api-argument", "The metadata input and output are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(metadata, 0, sizeof *metadata);
    edds_default_profile(&metadata->profile);
    if (!read_text(input, &source, &size, error)) return EDDS_INVALID_INPUT;
    scan.source = source;
    scan.size = size;
    scan.at = 0;
    value = next_token(&scan);
    if (value.kind != TOKEN_WORD || strcmp(value.text, "MetaFileClass") != 0 ||
        next_token(&scan).kind != TOKEN_OPEN) {
        fail(error, "malformed-metadata", "Metadata must contain one MetaFileClass block.");
        goto done;
    }
    for (;;) {
        token key = next_token(&scan);
        token field;
        if (key.kind == TOKEN_CLOSE) break;
        if (key.kind != TOKEN_WORD) {
            fail(error, "malformed-metadata", "The MetaFileClass block is malformed.");
            goto done;
        }
        field = next_token(&scan);
        if (strcmp(key.text, "Name") == 0) {
            if (found_name || field.kind != TOKEN_STRING || !parse_name(field.text, metadata)) {
                fail(error, "malformed-guid", "Metadata Name must begin with one 64-bit hexadecimal GUID.");
                goto done;
            }
            found_name = 1;
        } else if (strcmp(key.text, "Configurations") == 0) {
            edds_status configuration_status;
            if (found_configurations || field.kind != TOKEN_OPEN) {
                fail(error, "malformed-metadata", "Metadata must contain one Configurations block.");
                goto done;
            }
            found_configurations = 1;
            configuration_status = parse_configurations(&scan, metadata, &found_pc, error);
            if (configuration_status == EDDS_UNSUPPORTED_FORMAT) {
                pending = configuration_status;
                pending_error = *error;
            } else if (configuration_status != EDDS_OK) {
                status = configuration_status;
                goto done;
            }
        } else if (field.kind == TOKEN_OPEN && !skip_open_block(&scan)) {
            fail(error, "malformed-metadata", "An unknown metadata block is not closed.");
            goto done;
        }
    }
    if (next_token(&scan).kind != TOKEN_END || !found_name || !found_configurations || !found_pc) {
        fail(error, "incomplete-metadata", "Metadata requires Name and one source-image PC recipe.");
        goto done;
    }
    {
        const edds_source_capability *capability =
            edds_source_capability_of_format(metadata->source_format);
        if (capability == NULL || !extension_is(metadata->source_file, capability->extension)) {
            fail(error, "source-format-mismatch",
                "The PC resource class must match the SourceFile extension.");
            goto done;
        }
    }
    status = pending;
    if (pending != EDDS_OK) *error = pending_error;

done:
    free(source);
    return status;
}

static const char *compress_name(edds_format_compress compress) {
    switch (compress) {
        case EDDS_COMPRESS_COPY: return "Copy";
        case EDDS_COMPRESS_FASTEST: return "Fastest";
        case EDDS_COMPRESS_MEDIUM: return "Medium";
        case EDDS_COMPRESS_BEST: return "Best";
        default: return NULL;
    }
}

static int safe_string(const char *value) {
    if (*value == '\0') return 0;
    while (*value != '\0') {
        if (*value == '"' || (unsigned char)*value < 0x20u) return 0;
        ++value;
    }
    return 1;
}

edds_status edds_metadata_write(FILE *output, const edds_metadata *metadata, edds_error *error) {
    const char *resource;
    const char *compress;
    const edds_conversion_capability *conversion;
    char quality[8];
    int written;
    if (output == NULL || metadata == NULL) {
        fail(error, "invalid-api-argument", "The metadata value and output are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    {
        const edds_source_capability *capability =
            edds_source_capability_of_format(metadata->source_format);
        resource = capability == NULL ? NULL : capability->resource_class;
    }
    compress = compress_name(metadata->profile.format_compress);
    conversion = edds_conversion_capability_of(metadata->profile.conversion);
    if (resource == NULL || compress == NULL || conversion == NULL || strlen(metadata->guid) != 16u ||
        !safe_string(metadata->name) || !safe_string(metadata->source_file)) {
        fail(error, "invalid-metadata-value", "The structured metadata value is incomplete or unsupported.");
        return EDDS_INVALID_INPUT;
    }
    {
        /* Canonical metadata never records a recipe this converter would refuse to run. */
        const edds_status status = edds_profile_check(&metadata->profile, error);
        if (status != EDDS_OK) {
            return status;
        }
    }
    quality_text(metadata->profile.conversion_quality, quality);
    for (const char *at = metadata->guid; *at != '\0'; ++at) {
        if (!isxdigit((unsigned char)*at)) {
            fail(error, "malformed-guid", "The metadata GUID must be exactly 16 hexadecimal characters.");
            return EDDS_INVALID_INPUT;
        }
    }
    written = fprintf(output,
        "MetaFileClass {\n"
        " Name \"{%s}%s\"\n"
        " Configurations {\n"
        "  %s PC {\n"
        "   SourceFile \"%s\"\n"
        "   TargetFormat EnfusionDDS\n"
        "   FormatCompress %s\n"
        "   CompressTreshold %u\n"
        "   Conversion %s\n"
        "   ConversionQuality %s\n"
        "   Swizzling None\n"
        "   GenerateMips %d\n"
        "   MipMapFunction Filter\n"
        "   MipMapFilter Box\n"
        "   TiledTexture 1\n"
        "  }\n"
        "  %s XBOX_ONE : PC {\n"
        "  }\n"
        "  %s PS4 : PC {\n"
        "  }\n"
        "  %s LINUX : PC {\n"
        "  }\n"
        " }\n"
        "}\n",
        metadata->guid, metadata->name, resource, metadata->source_file, compress,
        metadata->profile.compress_threshold, conversion->workbench_name, quality,
        metadata->profile.generate_mips, resource, resource, resource);
    if (written < 0 || fflush(output) != 0) {
        fail(error, "metadata-write-failed", "Canonical metadata could not be written completely.");
        return EDDS_INTERNAL_FAILURE;
    }
    return EDDS_OK;
}
