/*
 * A texture's metadata file, read and written: one `MetaFileClass` block with a `Name` and a
 * `Configurations` block, whose PC recipe holds the conversion settings.
 */
#include "memory.h"
#include "meta_text.h"
#include <edds/edds.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/** One bit per recipe setting, to tell which settings have been read. */
enum setting_bit {
    SETTING_SOURCE        = 1u << 0,
    SETTING_TARGET        = 1u << 1,
    SETTING_FORMAT        = 1u << 2,
    SETTING_THRESHOLD     = 1u << 3,
    SETTING_REMOVE_MIPS   = 1u << 4,
    SETTING_CONVERSION    = 1u << 5,
    SETTING_QUALITY       = 1u << 6,
    SETTING_SWIZZLING     = 1u << 7,
    SETTING_CONTAINS_MIPS = 1u << 8,
    SETTING_GENERATE_MIPS = 1u << 9,
    SETTING_NORMALIZE     = 1u << 10,
    SETTING_MIP_FUNCTION  = 1u << 11,
    SETTING_MIP_FILTER    = 1u << 12,
    SETTING_TILED         = 1u << 13,
    SETTING_CUBEMAP       = 1u << 14
};

/** Fills `error` with a code and a printf-style message; does nothing when `error` is NULL. */
static void fail(edds_error *error, const char *code, const char *format, ...) {
    va_list arguments;

    if (error == NULL) {
        return;
    }

    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof error->message, format, arguments);
    va_end(arguments);
}

/** Reads a word of decimal digits that fits 32 bits into `result`; returns 0 for anything else. */
static int unsigned_value(const meta_token *value, uint32_t *result) {
    uint64_t parsed = 0;

    if (value->kind != META_TOKEN_WORD || value->text[0] == '\0') {
        return 0;
    }

    for (const char *at = value->text; *at != '\0'; ++at) {
        if (*at < '0' || *at > '9') {
            return 0;
        }

        parsed = parsed * 10u + (unsigned)(*at - '0');

        if (parsed > UINT32_MAX) {
            return 0;
        }
    }

    *result = (uint32_t)parsed;

    return 1;
}

/**
 * `ConversionQuality` as thousandths. Workbench writes it as a plain `1` or as up to three
 * decimals, and those are exactly the values that survive a round trip through this converter;
 * a fourth decimal would have to be rounded on the way back out and is refused instead.
 */
static int quality_value(const meta_token *value, uint32_t *result) {
    const char *at       = value->text;
    uint32_t    whole    = 0;
    uint32_t    fraction = 0;
    unsigned    digits   = 0;

    if (value->kind != META_TOKEN_WORD || *at < '0' || *at > '9') {
        return 0;
    }

    /* The whole part, at most 1. */
    while (*at >= '0' && *at <= '9') {
        whole = whole * 10u + (uint32_t)(*at - '0');

        if (whole > 1u) {
            return 0;
        }

        ++at;
    }

    /* The decimals after a point: one to three of them. */
    if (*at == '.') {
        ++at;

        if (*at < '0' || *at > '9') {
            return 0;
        }

        while (*at >= '0' && *at <= '9') {
            if (digits >= 3u) {
                return 0;
            }

            fraction = fraction * 10u + (uint32_t)(*at - '0');
            ++digits;
            ++at;
        }
    }

    if (*at != '\0') {
        return 0;
    }

    /* The decimals padded to three, so `0.5` is 500 thousandths. */
    while (digits < 3u) {
        fraction *= 10u;
        ++digits;
    }

    *result = whole * EDDS_QUALITY_SCALE + fraction;

    return *result <= EDDS_QUALITY_SCALE;
}

/** The shortest text that reads back as exactly this quality: `1`, `0.5`, `0.403`. */
static void quality_text(uint32_t value, char text[8]) {
    const uint32_t whole    = value / EDDS_QUALITY_SCALE;
    const uint32_t fraction = value % EDDS_QUALITY_SCALE;

    /* As many decimals as the value needs: none, one, two or three. */
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

/** Whether `value` is exactly one of the `count` strings in `choices`. */
static int is_one_of(const char *value, const char *const *choices, size_t count) {
    for (size_t at = 0; at < count; ++at) {
        if (strcmp(value, choices[at]) == 0) {
            return 1;
        }
    }

    return 0;
}

/** Refuses a setting that is recognized but unsupported, naming it and its value. */
static edds_status unsupported(edds_error *error, const char *key, const char *value) {
    fail(error, "unsupported-setting", "Workbench setting %s=%s is recognized but unsupported.",
        key, value);

    return EDDS_UNSUPPORTED_FORMAT;
}

/**
 * Applies one setting of the PC recipe to `metadata`. A key it does not know is skipped; a known
 * key may occur once, and `seen` records which have. A value it cannot read is malformed, and one
 * it recognizes but does not support is refused with `EDDS_UNSUPPORTED_FORMAT`.
 */
static edds_status recipe_setting(
    edds_metadata    *metadata,
    uint32_t         *seen,
    const meta_token *key,
    const meta_token *value,
    edds_error       *error) {
    uint32_t bit    = 0;
    uint32_t number = 0;

    /* Which setting the key names; any other key is left alone. */
    if (strcmp(key->text, "SourceFile") == 0) {
        bit = SETTING_SOURCE;
    } else if (strcmp(key->text, "TargetFormat") == 0) {
        bit = SETTING_TARGET;
    } else if (strcmp(key->text, "FormatCompress") == 0) {
        bit = SETTING_FORMAT;
    } else if (strcmp(key->text, "CompressTreshold") == 0) {
        bit = SETTING_THRESHOLD;
    } else if (strcmp(key->text, "RemoveMips") == 0) {
        bit = SETTING_REMOVE_MIPS;
    } else if (strcmp(key->text, "Conversion") == 0) {
        bit = SETTING_CONVERSION;
    } else if (strcmp(key->text, "ConversionQuality") == 0) {
        bit = SETTING_QUALITY;
    } else if (strcmp(key->text, "Swizzling") == 0) {
        bit = SETTING_SWIZZLING;
    } else if (strcmp(key->text, "ContainsMips") == 0) {
        bit = SETTING_CONTAINS_MIPS;
    } else if (strcmp(key->text, "GenerateMips") == 0) {
        bit = SETTING_GENERATE_MIPS;
    } else if (strcmp(key->text, "Normalize") == 0) {
        bit = SETTING_NORMALIZE;
    } else if (strcmp(key->text, "MipMapFunction") == 0) {
        bit = SETTING_MIP_FUNCTION;
    } else if (strcmp(key->text, "MipMapFilter") == 0) {
        bit = SETTING_MIP_FILTER;
    } else if (strcmp(key->text, "TiledTexture") == 0) {
        bit = SETTING_TILED;
    } else if (strcmp(key->text, "GenerateCubemap") == 0) {
        bit = SETTING_CUBEMAP;
    } else {
        return EDDS_OK;
    }

    if ((*seen & bit) != 0) {
        fail(error, "duplicate-setting", "Workbench setting %s occurs more than once in PC.",
            key->text);
        return EDDS_INVALID_INPUT;
    }

    *seen |= bit;

    /* `SourceFile` takes a quoted string; every other setting takes a word. */
    if (bit == SETTING_SOURCE) {
        if (value->kind != META_TOKEN_STRING ||
            !meta_copy_text(metadata->source_file, sizeof metadata->source_file, value->text)) {
            goto malformed;
        }

        return EDDS_OK;
    }

    if (value->kind != META_TOKEN_WORD) {
        goto malformed;
    }

    /* `TargetFormat`: one of four names, of which only `EnfusionDDS` is supported. */
    if (bit == SETTING_TARGET) {
        static const char *const known[] = {
            "EnfusionDDS", "EnfusionDDS_LZ0", "EnfusionDDS_LZ4", "DirectXDDS"
        };

        if (!is_one_of(value->text, known, sizeof known / sizeof known[0])) {
            goto malformed;
        }

        return strcmp(value->text, "EnfusionDDS") == 0
            ? EDDS_OK
            : unsupported(error, key->text, value->text);
    }

    if (bit == SETTING_FORMAT) {
        if (strcmp(value->text, "Copy") == 0) {
            metadata->profile.format_compress = EDDS_COMPRESS_COPY;
        } else if (strcmp(value->text, "Fastest") == 0) {
            metadata->profile.format_compress = EDDS_COMPRESS_FASTEST;
        } else if (strcmp(value->text, "Medium") == 0) {
            metadata->profile.format_compress = EDDS_COMPRESS_MEDIUM;
        } else if (strcmp(value->text, "Best") == 0) {
            metadata->profile.format_compress = EDDS_COMPRESS_BEST;
        } else {
            goto malformed;
        }

        return EDDS_OK;
    }

    /* `Conversion`: a name the conversion table knows, refused when it is not supported. */
    if (bit == SETTING_CONVERSION) {
        const edds_conversion_capability *capability =
            edds_conversion_of_workbench_name(value->text);

        if (capability == NULL) {
            goto malformed;
        }

        metadata->profile.conversion = capability->conversion;
        return capability->supported ? EDDS_OK : unsupported(error, key->text, value->text);
    }

    if (bit == SETTING_QUALITY) {
        if (!quality_value(value, &metadata->profile.conversion_quality)) {
            goto malformed;
        }

        return EDDS_OK;
    }

    /* `Swizzling`: a name the swizzle table does not know is unsupported rather than malformed. */
    if (bit == SETTING_SWIZZLING) {
        const edds_swizzle_capability *capability = edds_swizzle_of_workbench_name(value->text);

        if (capability == NULL) {
            return unsupported(error, key->text, value->text);
        }

        metadata->profile.swizzling = capability->swizzling;
        return EDDS_OK;
    }

    if (bit == SETTING_MIP_FUNCTION) {
        static const char *const known[] = { "Filter", "ColorNoise", "Normalize" };

        if (!is_one_of(value->text, known, sizeof known / sizeof known[0])) {
            goto malformed;
        }

        if (strcmp(value->text, "Filter") == 0) {
            metadata->profile.mipmap_function = EDDS_MIPMAP_FILTER;
        } else if (strcmp(value->text, "Normalize") == 0) {
            metadata->profile.mipmap_function = EDDS_MIPMAP_NORMALIZE;
        } else {
            metadata->profile.mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
        }

        return EDDS_OK;
    }

    if (bit == SETTING_MIP_FILTER) {
        static const char *const known[] = { "Box", "Kaiser", "Triangle" };

        if (!is_one_of(value->text, known, sizeof known / sizeof known[0])) {
            goto malformed;
        }

        if (strcmp(value->text, "Box") == 0) {
            metadata->profile.mipmap_filter = EDDS_FILTER_BOX;
        } else if (strcmp(value->text, "Kaiser") == 0) {
            metadata->profile.mipmap_filter = EDDS_FILTER_KAISER;
        } else {
            metadata->profile.mipmap_filter = EDDS_FILTER_TRIANGLE;
        }

        return EDDS_OK;
    }

    /*
     * The rest are whole numbers: a percentage up to 100, a count of mips, and switches that are 0
     * or 1. `GenerateCubemap` is supported only as 0.
     */
    if (!unsigned_value(value, &number)) {
        goto malformed;
    }

    if (bit == SETTING_THRESHOLD) {
        if (number > 100u) {
            return unsupported(error, key->text, value->text);
        }

        metadata->profile.compress_threshold = number;
    } else if (bit == SETTING_REMOVE_MIPS) {
        metadata->profile.remove_mips = number;
    } else if (bit == SETTING_CONTAINS_MIPS) {
        if (number > 1u) {
            goto malformed;
        }

        metadata->profile.contains_mips = number != 0;
    } else if (bit == SETTING_GENERATE_MIPS) {
        if (number > 1u) {
            goto malformed;
        }

        metadata->profile.generate_mips = number != 0;
    } else if (bit == SETTING_NORMALIZE) {
        if (number > 1u) {
            goto malformed;
        }

        metadata->profile.normalize = number != 0;
    } else if (bit == SETTING_CUBEMAP && number != 0u) {
        return unsupported(error, key->text, value->text);
    } else if (bit == SETTING_TILED) {
        if (number > 1u) {
            goto malformed;
        }

        metadata->profile.tiled_texture = number != 0;
    }

    return EDDS_OK;

malformed:
    fail(error, "malformed-setting", "Workbench setting %s has a malformed or unknown value.",
        key->text);
    return EDDS_INVALID_INPUT;
}

/**
 * Reads the settings of the PC recipe into `metadata`, up to the brace that closes it. A malformed
 * setting stops the reading at once; the first unsupported one is held back and returned only
 * after the whole recipe has been read and checked as one profile.
 */
static edds_status parse_recipe(
    meta_scanner  *scan,
    edds_metadata *metadata,
    edds_error    *error) {
    uint32_t    seen          = 0;
    edds_status pending       = EDDS_OK;
    edds_error  pending_error = { { 0 }, { 0 } };

    /* The settings, a key and its value at a time. */
    for (;;) {
        const meta_token key = meta_next_token(scan);
        meta_token       value;
        edds_status      status;

        if (key.kind == META_TOKEN_CLOSE) {
            break;
        }

        if (key.kind != META_TOKEN_WORD) {
            fail(error, "malformed-metadata", "The PC recipe is malformed.");
            return EDDS_INVALID_INPUT;
        }

        value = meta_next_token(scan);

        /* A key followed by a block is skipped, block and all. */
        if (value.kind == META_TOKEN_OPEN) {
            if (!meta_skip_open_block(scan)) {
                fail(error, "malformed-metadata", "A metadata block is not closed.");
                return EDDS_INVALID_INPUT;
            }

            continue;
        }

        status = recipe_setting(metadata, &seen, &key, &value, error);

        if (status == EDDS_UNSUPPORTED_FORMAT) {
            if (pending == EDDS_OK) {
                pending_error = *error;
            }

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
     * Settings arrive in whatever order the file wrote them, so a combination of two of them -- a
     * quality against the conversion it would reach -- is only decidable once the recipe is whole.
     */
    {
        edds_error  combination;
        edds_status status = edds_profile_check(&metadata->profile, &combination);

        if (status == EDDS_OK &&
            metadata->source_format != EDDS_SOURCE_DDS &&
            metadata->profile.contains_mips) {
            fail(&combination, "unsupported-combination",
                "ContainsMips is supported only for a DDS source.");
            status = EDDS_UNSUPPORTED_FORMAT;
        }

        if (status != EDDS_OK && pending == EDDS_OK) {
            pending       = status;
            pending_error = combination;
        }
    }

    if (pending != EDDS_OK) {
        *error = pending_error;
    }

    return pending;
}

/**
 * Reads the `Configurations` block up to its closing brace: one block per resource class and
 * platform, of which only the PC recipe of a source resource class is read and the rest skipped.
 * A second such recipe is refused; an unsupported setting is returned only at the end.
 */
static edds_status parse_configurations(
    meta_scanner  *scan,
    edds_metadata *metadata,
    int           *found_pc,
    edds_error    *error) {
    edds_status pending       = EDDS_OK;
    edds_error  pending_error = { { 0 }, { 0 } };

    for (;;) {
        meta_token                    resource = meta_next_token(scan);
        meta_token                    platform;
        meta_token                    next;
        const edds_source_capability *recognized;

        if (resource.kind == META_TOKEN_CLOSE) {
            if (pending != EDDS_OK) {
                *error = pending_error;
            }

            return pending;
        }

        /* A block opens with a resource class and a platform, and `: Parent` when it inherits. */
        if (resource.kind != META_TOKEN_WORD) {
            goto malformed;
        }

        platform = meta_next_token(scan);

        if (platform.kind != META_TOKEN_WORD) {
            goto malformed;
        }

        next = meta_next_token(scan);

        if (next.kind == META_TOKEN_COLON) {
            if (meta_next_token(scan).kind != META_TOKEN_WORD) {
                goto malformed;
            }

            next = meta_next_token(scan);
        }

        if (next.kind != META_TOKEN_OPEN) {
            goto malformed;
        }

        /* The PC block of a source resource class is the recipe; any other block is skipped. */
        recognized = edds_source_capability_of_resource_class(resource.text);

        if (recognized != NULL && strcmp(platform.text, "PC") == 0) {
            edds_status status;

            if (*found_pc) {
                fail(error, "duplicate-pc-recipe",
                    "Metadata contains more than one source-image PC recipe.");
                return EDDS_INVALID_INPUT;
            }

            *found_pc               = 1;
            metadata->source_format = recognized->format;

            status = parse_recipe(scan, metadata, error);

            if (status == EDDS_UNSUPPORTED_FORMAT) {
                if (pending == EDDS_OK) {
                    pending_error = *error;
                }

                pending = EDDS_UNSUPPORTED_FORMAT;
            } else if (status != EDDS_OK) {
                return status;
            }
        } else if (!meta_skip_open_block(scan)) {
            goto malformed;
        }
    }

malformed:
    fail(error, "malformed-metadata", "The Configurations block is malformed.");
    return EDDS_INVALID_INPUT;
}

/** Whether `path` ends in `extension`, the letters of the path taken in lower case. */
static int extension_is(const char *path, const char *extension) {
    const size_t path_size      = strlen(path);
    const size_t extension_size = strlen(extension);

    if (path_size < extension_size) {
        return 0;
    }

    for (size_t at = 0; at < extension_size; ++at) {
        const unsigned char value = (unsigned char)path[path_size - extension_size + at];

        if (tolower(value) != (unsigned char)extension[at]) {
            return 0;
        }
    }

    return 1;
}

/**
 * Reads a texture metadata file into `metadata`: one `MetaFileClass` block, holding one `Name` and
 * one `Configurations` block with exactly one PC recipe for a source image, whose resource class
 * must match the extension of its `SourceFile`. Other blocks are skipped. A setting that is
 * recognized but unsupported is reported only when nothing in the file is malformed.
 */
edds_status edds_metadata_parse(FILE *input, edds_metadata *metadata, edds_error *error) {
    /* The text of the file, and the scanner that reads it. */
    char        *source = NULL;
    size_t       size   = 0;
    meta_scanner scan;
    meta_token   value;

    /* Which of the required entries have been met so far. */
    int found_name           = 0;
    int found_configurations = 0;
    int found_pc             = 0;

    /* The result, and an unsupported setting held back until the whole file has been read. */
    edds_status status        = EDDS_INVALID_INPUT;
    edds_status pending       = EDDS_OK;
    edds_error  pending_error = { { 0 }, { 0 } };

    if (input == NULL || metadata == NULL) {
        fail(error, "invalid-api-argument", "The metadata input and output are required.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* Settings the recipe does not mention keep their defaults. */
    memset(metadata, 0, sizeof *metadata);
    edds_default_profile(&metadata->profile);

    if (!meta_read_text(input, &source, &size, error)) {
        return EDDS_INVALID_INPUT;
    }

    /* The file is one `MetaFileClass` block. */
    scan.source = source;
    scan.size   = size;
    scan.at     = 0;
    value       = meta_next_token(&scan);

    if (value.kind != META_TOKEN_WORD ||
        strcmp(value.text, "MetaFileClass") != 0 ||
        meta_next_token(&scan).kind != META_TOKEN_OPEN) {
        fail(error, "malformed-metadata", "Metadata must contain one MetaFileClass block.");
        goto done;
    }

    /* Its entries, up to its closing brace. */
    for (;;) {
        meta_token key = meta_next_token(&scan);
        meta_token field;

        if (key.kind == META_TOKEN_CLOSE) {
            break;
        }

        if (key.kind != META_TOKEN_WORD) {
            fail(error, "malformed-metadata", "The MetaFileClass block is malformed.");
            goto done;
        }

        field = meta_next_token(&scan);

        if (strcmp(key.text, "Name") == 0) {
            /* `Name` once: a string of the GUID in braces, then the resource path. */
            if (found_name ||
                field.kind != META_TOKEN_STRING ||
                !meta_parse_name(field.text, metadata->guid, metadata->name,
                    sizeof metadata->name)) {
                fail(error, "malformed-guid",
                    "Metadata Name must begin with one 64-bit hexadecimal GUID.");
                goto done;
            }

            found_name = 1;
        } else if (strcmp(key.text, "Configurations") == 0) {
            /* `Configurations` once: a block, whose unsupported setting is held back. */
            edds_status configuration_status;

            if (found_configurations || field.kind != META_TOKEN_OPEN) {
                fail(error, "malformed-metadata",
                    "Metadata must contain one Configurations block.");
                goto done;
            }

            found_configurations = 1;
            configuration_status = parse_configurations(&scan, metadata, &found_pc, error);

            if (configuration_status == EDDS_UNSUPPORTED_FORMAT) {
                pending       = configuration_status;
                pending_error = *error;
            } else if (configuration_status != EDDS_OK) {
                status = configuration_status;
                goto done;
            }
        } else if (field.kind == META_TOKEN_OPEN && !meta_skip_open_block(&scan)) {
            fail(error, "malformed-metadata", "An unknown metadata block is not closed.");
            goto done;
        }
    }

    /* Nothing after the block, and every entry the file must have. */
    if (meta_next_token(&scan).kind != META_TOKEN_END ||
        !found_name ||
        !found_configurations ||
        !found_pc) {
        fail(error, "incomplete-metadata",
            "Metadata requires Name and one source-image PC recipe.");
        goto done;
    }

    /* The resource class of the PC recipe must match the extension of its `SourceFile`. */
    {
        const edds_source_capability *capability =
            edds_source_capability_of_format(metadata->source_format);

        if (capability == NULL || !extension_is(metadata->source_file, capability->extension)) {
            fail(error, "source-format-mismatch",
                "The PC resource class must match the SourceFile extension.");
            goto done;
        }
    }

    /* The file is well formed: the result is the unsupported setting held back, if any. */
    status = pending;

    if (pending != EDDS_OK) {
        *error = pending_error;
    }

done:
    edds_free(source);
    return status;
}

/** The Workbench name of a `FormatCompress` value, or NULL for a value outside the enum. */
static const char *compress_name(edds_format_compress compress) {
    switch (compress) {
        case EDDS_COMPRESS_COPY:    return "Copy";
        case EDDS_COMPRESS_FASTEST: return "Fastest";
        case EDDS_COMPRESS_MEDIUM:  return "Medium";
        case EDDS_COMPRESS_BEST:    return "Best";

        default: return NULL;
    }
}

/** The Workbench name of a `MipMapFunction` value, or NULL for a value outside the enum. */
static const char *mipmap_function_name(edds_mipmap_function function) {
    switch (function) {
        case EDDS_MIPMAP_FILTER:      return "Filter";
        case EDDS_MIPMAP_NORMALIZE:   return "Normalize";
        case EDDS_MIPMAP_COLOR_NOISE: return "ColorNoise";

        default: return NULL;
    }
}

/** The Workbench name of a `MipMapFilter` value, or NULL for a value outside the enum. */
static const char *mipmap_filter_name(edds_mipmap_filter filter) {
    switch (filter) {
        case EDDS_FILTER_BOX:      return "Box";
        case EDDS_FILTER_KAISER:   return "Kaiser";
        case EDDS_FILTER_TRIANGLE: return "Triangle";

        default: return NULL;
    }
}

/**
 * A value written between quotes as it is. The reader takes `\"` and `\\` for escapes, so a
 * backslash -- a name ending in one would swallow its closing quote -- is refused, as the font
 * recipe refuses it; resource paths in Workbench metadata use forward slashes.
 */
static int safe_string(const char *value) {
    if (*value == '\0') {
        return 0;
    }

    while (*value != '\0') {
        if (*value == '"' || *value == '\\' || (unsigned char)*value < 0x20u) {
            return 0;
        }

        ++value;
    }

    return 1;
}

/**
 * Writes `metadata` as a texture metadata file of one fixed layout: every setting in the PC
 * recipe, and the XBOX_ONE, PS4 and LINUX recipes inheriting from it. Refuses a value it cannot
 * write, and a recipe this converter would refuse to run.
 */
edds_status edds_metadata_write(FILE *output, const edds_metadata *metadata, edds_error *error) {
    /* The Workbench names of the resource class and of the settings that are enums. */
    const char                       *resource;
    const char                       *compress;
    const char                       *mipmap_function;
    const char                       *mipmap_filter;
    const edds_conversion_capability *conversion;

    /* The quality as text, and what the write returns. */
    char quality[8];
    int  written;

    if (output == NULL || metadata == NULL) {
        fail(error, "invalid-api-argument", "The metadata value and output are required.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* The names; a NULL among them means a value outside its enum or table. */
    {
        const edds_source_capability *capability =
            edds_source_capability_of_format(metadata->source_format);

        resource = capability == NULL ? NULL : capability->resource_class;
    }

    compress        = compress_name(metadata->profile.format_compress);
    mipmap_function = mipmap_function_name(metadata->profile.mipmap_function);
    mipmap_filter   = mipmap_filter_name(metadata->profile.mipmap_filter);
    conversion      = edds_conversion_capability_of(metadata->profile.conversion);

    if (resource == NULL ||
        compress == NULL ||
        conversion == NULL ||
        mipmap_function == NULL ||
        mipmap_filter == NULL ||
        strlen(metadata->guid) != 16u ||
        !safe_string(metadata->name) ||
        !safe_string(metadata->source_file)) {
        fail(error, "invalid-metadata-value",
            "The structured metadata value is incomplete or unsupported.");
        return EDDS_INVALID_INPUT;
    }

    {
        /* Canonical metadata never records a recipe this converter would refuse to run. */
        const edds_status status = edds_profile_check(&metadata->profile, error);

        if (status != EDDS_OK) {
            return status;
        }

        if (metadata->source_format != EDDS_SOURCE_DDS && metadata->profile.contains_mips) {
            fail(error, "unsupported-combination",
                "ContainsMips is supported only for a DDS source.");
            return EDDS_UNSUPPORTED_FORMAT;
        }
    }

    quality_text(metadata->profile.conversion_quality, quality);

    if (!meta_valid_guid(metadata->guid)) {
        fail(error, "malformed-guid",
            "The metadata GUID must be exactly 16 hexadecimal characters.");
        return EDDS_INVALID_INPUT;
    }

    /* The whole file in one write; the values below are listed one line of the text at a time. */
    written = fprintf(output,
        "MetaFileClass {\n"
        " Name \"{%s}%s\"\n"
        " Configurations {\n"
        "  %s PC {\n"
        "   SourceFile \"%s\"\n"
        "   TargetFormat EnfusionDDS\n"
        "   FormatCompress %s\n"
        "   CompressTreshold %u\n"
        "   RemoveMips %u\n"
        "   Conversion %s\n"
        "   ConversionQuality %s\n"
        "   Swizzling %s\n"
        "   ContainsMips %d\n"
        "   GenerateMips %d\n"
        "   Normalize %d\n"
        "   MipMapFunction %s\n"
        "   MipMapFilter %s\n"
        "   TiledTexture %d\n"
        "  }\n"
        "  %s XBOX_ONE : PC {\n"
        "  }\n"
        "  %s PS4 : PC {\n"
        "  }\n"
        "  %s LINUX : PC {\n"
        "  }\n"
        " }\n"
        "}\n",
        metadata->guid, metadata->name,
        resource,
        metadata->source_file,
        compress,
        metadata->profile.compress_threshold,
        metadata->profile.remove_mips,
        conversion->workbench_name,
        quality,
        edds_swizzle_capability_of(metadata->profile.swizzling)->workbench_name,
        metadata->profile.contains_mips,
        metadata->profile.generate_mips,
        metadata->profile.normalize,
        mipmap_function,
        mipmap_filter,
        metadata->profile.tiled_texture,
        resource,
        resource,
        resource);

    if (written < 0 || fflush(output) != 0) {
        fail(error, "metadata-write-failed", "Canonical metadata could not be written completely.");
        return EDDS_INTERNAL_FAILURE;
    }

    return EDDS_OK;
}
