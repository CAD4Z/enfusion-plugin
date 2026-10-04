/*
 * `enfusion edds ...`: inspect, preview, convert and batch, their flags and their JSON.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "cli.h"

#include <edds/batch.h>
#include <edds/pool.h>

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/** The one `--protocol` number this area accepts. */
enum {
    EDDS_PROTOCOL_VERSION = 1
};

/** The same number as text, for `batch`, whose command line is matched word for word. */
#define EDDS_PROTOCOL_TEXT "1"

/**
 * `--conversion-quality` on the command line, in thousandths. The CLI takes the same text the
 * Workbench recipe holds (`1`, `0.5`, `0.403`), so a profile can be handed straight across.
 * Returns 1 with the value, or 0 for text that is not a number from 0 to 1 with at most three
 * decimals.
 */
static int quality_argument(const cli_char *text, uint32_t *value) {
    uint32_t whole    = 0;
    uint32_t fraction = 0;
    unsigned digits   = 0;
    int      seen     = 0;

    /* The whole part: at least one digit, and no more than 1. */
    while (*text >= (cli_char)'0' && *text <= (cli_char)'9') {
        whole = whole * 10u + (uint32_t)(*text++ - (cli_char)'0');

        if (whole > 1u) {
            return 0;
        }

        seen = 1;
    }

    if (!seen) {
        return 0;
    }

    /* The fraction, if any: a dot, then one to three digits. */
    if (*text == (cli_char)'.') {
        ++text;

        if (*text < (cli_char)'0' || *text > (cli_char)'9') {
            return 0;
        }

        while (*text >= (cli_char)'0' && *text <= (cli_char)'9') {
            if (digits >= 3u) {
                return 0;
            }

            fraction = fraction * 10u + (uint32_t)(*text++ - (cli_char)'0');
            ++digits;
        }
    }

    /* Nothing may follow. */
    if (*text != 0) {
        return 0;
    }

    /* The fraction in thousandths: `0.5` is 500. */
    while (digits < 3u) {
        fraction *= 10u;
        ++digits;
    }

    *value = whole * EDDS_QUALITY_SCALE + fraction;

    return *value <= EDDS_QUALITY_SCALE;
}

/** Prints the usage of the edds area to stderr. */
static void usage(void) {
    fputs(
        "usage:\n"
        "  enfusion edds inspect --machine --protocol 1 --input PATH [--metadata PATH.meta [--identity-only]]\n"
        "  enfusion edds preview --machine --protocol 1 --mip N --input PATH\n"
        "  enfusion edds batch --machine --protocol 1 [--cancel-file PATH] < jobs.ndjson\n"
        "  enfusion edds convert --machine --protocol 1 --input PATH --output PATH [PROFILE FLAGS]\n"
        "      [--metadata PATH.meta --resource-name NAME --source-file NAME --guid HEX]\n"
        "      [--expect-source-revision R --expect-output-revision R --expect-metadata-revision R]\n"
        "      [--cancel-file PATH]\n",
        stderr);
}

/**
 * Refuses a command line that does not fit: the usage on stderr, and the error envelope on
 * stdout. Returns the exit code.
 */
static int invalid_invocation(const char *code, const char *message) {
    edds_error error;

    memset(&error, 0, sizeof error);
    (void)snprintf(error.code, sizeof error.code, "%s", code);
    (void)snprintf(error.message, sizeof error.message, "%s", message);

    usage();

    return report_failure(EDDS_INVALID_INVOCATION, &error);
}

/**
 * The name of the file's pixel format. A DXGI format is named by its number, `DXGI_<n>`, which is
 * written into `buffer`.
 */
static const char *pixel_format(const edds_info *info, char buffer[32]) {
    if (info->pixel_format == EDDS_PIXEL_DXGI) {
        (void)snprintf(buffer, 32, "DXGI_%u", info->dxgi_format);
        return buffer;
    }

    return edds_pixel_format_name(info->pixel_format);
}

/** Why there is no preview of this file, as a sentence for the user. */
static const char *preview_refusal(const edds_info *info) {
    if (info->pixel_format == EDDS_PIXEL_DXGI || info->pixel_format == EDDS_PIXEL_UNKNOWN) {
        return "Pixel preview is unavailable because this DDS pixel format is not supported.";
    }

    if ((uint64_t)info->width * info->height * 4u > EDDS_MAX_PREVIEW_BYTES) {
        return "Pixel preview is unavailable because this texture decodes to more pixels than one preview holds.";
    }

    return "Pixel preview is unavailable because only one two-dimensional texture surface is supported.";
}

/** The recipe's name for a `FormatCompress` value; `Unknown` for any other. */
static const char *metadata_compress(edds_format_compress compress) {
    switch (compress) {
        case EDDS_COMPRESS_COPY:    return "Copy";
        case EDDS_COMPRESS_FASTEST: return "Fastest";
        case EDDS_COMPRESS_MEDIUM:  return "Medium";
        case EDDS_COMPRESS_BEST:    return "Best";
        default:                    return "Unknown";
    }
}

/** The recipe's name for a `MipMapFunction` value; `Unknown` for any other. */
static const char *metadata_mipmap_function(edds_mipmap_function function) {
    switch (function) {
        case EDDS_MIPMAP_FILTER:      return "Filter";
        case EDDS_MIPMAP_NORMALIZE:   return "Normalize";
        case EDDS_MIPMAP_COLOR_NOISE: return "ColorNoise";
        default:                      return "Unknown";
    }
}

/** The recipe's name for a `MipMapFilter` value; `Unknown` for any other. */
static const char *metadata_mipmap_filter(edds_mipmap_filter filter) {
    switch (filter) {
        case EDDS_FILTER_BOX:      return "Box";
        case EDDS_FILTER_KAISER:   return "Kaiser";
        case EDDS_FILTER_TRIANGLE: return "Triangle";
        default:                   return "Unknown";
    }
}

/**
 * The same shortest exact text the metadata carries, as a JSON number rather than a string.
 * Written into `buffer`, which is returned.
 */
static const char *quality_json(uint32_t value, char buffer[8]) {
    const uint32_t whole    = value / EDDS_QUALITY_SCALE;
    const uint32_t fraction = value % EDDS_QUALITY_SCALE;

    /* The thousandths without their trailing zeros: 500 is `0.5`, 30 is `0.03`. */
    if (fraction == 0) {
        (void)snprintf(buffer, 8, "%u", whole);
    } else if (fraction % 100u == 0) {
        (void)snprintf(buffer, 8, "%u.%u", whole, fraction / 100u);
    } else if (fraction % 10u == 0) {
        (void)snprintf(buffer, 8, "%u.%02u", whole, fraction / 10u);
    } else {
        (void)snprintf(buffer, 8, "%u.%03u", whole, fraction);
    }

    return buffer;
}

/**
 * The `inspect` JSON line on stdout: the texture's size, format and mips, its DDS header, and its
 * metadata when there is some. With `unsupported_metadata_reason`, only the metadata's identity
 * is written, under that reason.
 */
static void write_inspection(const edds_info *info, const edds_metadata *metadata, const char *unsupported_metadata_reason) {
    char        format_buffer[32];
    char        quality_buffer[8];
    /* The channels a decode of the file carries, which is the runtime fact, not the source's. */
    const char *channels = edds_pixel_format_channels(info->pixel_format);

    (void)printf(
        "{\"protocolVersion\":1,\"kind\":\"inspect\",\"width\":%u,\"height\":%u,"
        "\"mipCount\":%u,\"pixelFormat\":\"%s\",\"channels\":\"%s\",\"previewSupported\":%s",
        info->width, info->height, info->mip_count, pixel_format(info, format_buffer),
        channels, info->preview_supported ? "true" : "false");

    if (!info->preview_supported) {
        fputs(",\"previewUnsupportedReason\":", stdout);
        json_string(preview_refusal(info));
    }

    /* The DDS header, field by field. */
    (void)printf(
        ",\"dds\":{\"flags\":%u,\"pitchOrLinearSize\":%u,\"depth\":%u,"
        "\"pixelFormatFlags\":%u,\"fourCC\":",
        info->flags, info->pitch_or_linear_size, info->depth, info->pixel_format_flags);
    json_string(info->four_cc);
    (void)printf(
        ",\"rgbBitCount\":%u,\"rMask\":%u,\"gMask\":%u,\"bMask\":%u,\"aMask\":%u,"
        "\"caps\":%u,\"caps2\":%u,\"dxgiFormat\":%u,"
        "\"resourceDimension\":%u,\"arraySize\":%u,\"miscFlag\":%u},\"mips\":[",
        info->rgb_bit_count, info->r_mask, info->g_mask, info->b_mask, info->a_mask,
        info->caps, info->caps2,
        info->dxgi_format, info->resource_dimension, info->array_size, info->misc_flag);

    /* One object for each mip level. */
    for (uint32_t at = 0; at < info->mip_count; ++at) {
        const edds_mip *mip = &info->mips[at];

        if (at != 0) {
            putchar(',');
        }

        (void)printf(
            "{\"level\":%u,\"width\":%u,\"height\":%u,\"container\":\"%s\","
            "\"storedBytes\":%u,\"decodedBytes\":%u,\"blockCount\":%u,"
            "\"dataOffset\":%llu}",
            mip->level, mip->width, mip->height, edds_container_name(mip->container),
            mip->stored_bytes, mip->decoded_bytes, mip->block_count,
            (unsigned long long)mip->data_offset);
    }

    fputs("]", stdout);

    /* The metadata: only its identity when its recipe is refused, all of it otherwise. */
    if (metadata != NULL && unsupported_metadata_reason != NULL) {
        fputs(",\"unsupportedMetadata\":{\"reason\":", stdout);
        json_string(unsupported_metadata_reason);
        fputs(",\"identity\":{\"guid\":", stdout);
        json_string(metadata->guid);
        fputs(",\"name\":", stdout);
        json_string(metadata->name);
        fputs(",\"sourceFile\":", stdout);
        json_string(metadata->source_file);
        fputs("}}", stdout);
    } else if (metadata != NULL) {
        fputs(",\"metadata\":{\"schemaVersion\":1,\"identity\":{\"guid\":", stdout);
        json_string(metadata->guid);
        fputs(",\"name\":", stdout);
        json_string(metadata->name);
        fputs(",\"sourceFile\":", stdout);
        json_string(metadata->source_file);
        fputs(",\"sourceFormat\":", stdout);

        {
            const edds_source_capability *capability = edds_source_capability_of_format(metadata->source_format);

            json_string(capability == NULL ? "" : capability->wire_name);
        }

        /* The recipe, each field under its own name in the recipe. */
        fputs("},\"recipe\":{\"TargetFormat\":\"EnfusionDDS\",\"FormatCompress\":", stdout);
        json_string(metadata_compress(metadata->profile.format_compress));
        (void)printf(",\"CompressTreshold\":%u,\"RemoveMips\":%u,\"Conversion\":",
            metadata->profile.compress_threshold, metadata->profile.remove_mips);

        {
            const edds_conversion_capability *conversion = edds_conversion_capability_of(metadata->profile.conversion);

            json_string(conversion == NULL ? "None" : conversion->workbench_name);
        }

        (void)printf(
            ",\"ConversionQuality\":%s,\"Swizzling\":\"%s\","
            "\"ContainsMips\":%s,\"GenerateMips\":%s,\"Normalize\":%s,"
            "\"MipMapFunction\":",
            quality_json(metadata->profile.conversion_quality, quality_buffer),
            edds_swizzle_capability_of(metadata->profile.swizzling)->workbench_name,
            metadata->profile.contains_mips ? "true" : "false",
            metadata->profile.generate_mips ? "true" : "false",
            metadata->profile.normalize ? "true" : "false");
        json_string(metadata_mipmap_function(metadata->profile.mipmap_function));
        fputs(",\"MipMapFilter\":", stdout);
        json_string(metadata_mipmap_filter(metadata->profile.mipmap_filter));
        (void)printf(",\"TiledTexture\":%s}}", metadata->profile.tiled_texture ? "true" : "false");
    }

    fputs("}\n", stdout);
}

/** Writes `bytes` to stdout as Base64, padded with `=`. */
static void write_base64(const uint8_t *bytes, size_t size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t            at         = 0;

    /* Every three bytes become four characters of six bits each. */
    while (size - at >= 3) {
        const uint32_t value = ((uint32_t)bytes[at] << 16) | ((uint32_t)bytes[at + 1] << 8) | bytes[at + 2];

        putchar(alphabet[(value >> 18) & 63u]);
        putchar(alphabet[(value >> 12) & 63u]);
        putchar(alphabet[(value >> 6) & 63u]);
        putchar(alphabet[value & 63u]);
        at += 3;
    }

    /* One or two bytes left over: two or three characters, then the padding. */
    if (size - at == 1) {
        const uint32_t value = (uint32_t)bytes[at] << 16;

        putchar(alphabet[(value >> 18) & 63u]);
        putchar(alphabet[(value >> 12) & 63u]);
        fputs("==", stdout);
    } else if (size - at == 2) {
        const uint32_t value = ((uint32_t)bytes[at] << 16) | ((uint32_t)bytes[at + 1] << 8);

        putchar(alphabet[(value >> 18) & 63u]);
        putchar(alphabet[(value >> 12) & 63u]);
        putchar(alphabet[(value >> 6) & 63u]);
        putchar('=');
    }
}

/** The `preview` JSON line on stdout: the size of one mip level and its RGBA pixels in Base64. */
static void write_preview(const edds_info *info, uint32_t mip, const uint8_t *rgba, size_t size) {
    const edds_mip *selected = &info->mips[mip];

    (void)printf(
        "{\"protocolVersion\":1,\"kind\":\"preview\",\"mip\":%u,"
        "\"width\":%u,\"height\":%u,\"pixelFormat\":\"RGBA8\","
        "\"byteLength\":%llu,\"pixelsBase64\":\"",
        mip, selected->width, selected->height, (unsigned long long)size);
    write_base64(rgba, size);
    fputs("\"}\n", stdout);
}

/** The flags of an edds command; a flag that was not given stays 0 or NULL. */
typedef struct parsed_arguments {
    /** `--machine`, and `--protocol` with its number. */
    int      machine;
    int      protocol_seen;
    uint32_t protocol;

    /** `--mip` with its number. */
    int      mip_seen;
    uint32_t mip;

    /** `--input` and `--output`. */
    const cli_char *input;
    const cli_char *output;

    /** The profile flags, as given; a number comes with whether it was given. */
    const cli_char *target_format;
    const cli_char *format_compress;
    int             threshold_seen;
    uint32_t        compress_threshold;
    int             remove_mips_seen;
    uint32_t        remove_mips;
    const cli_char *conversion;
    int             quality_seen;
    uint32_t        conversion_quality;
    const cli_char *swizzling;
    const cli_char *contains_mips;
    const cli_char *generate_mips;
    const cli_char *normalize;
    const cli_char *mipmap_function;
    const cli_char *mipmap_filter;
    const cli_char *tiled_texture;

    /** `--metadata`, and the identity it is written with. */
    const cli_char *metadata;
    const cli_char *resource_name;
    const cli_char *source_file;
    const cli_char *guid;

    /** The revisions the caller planned with: `--expect-source-revision` and the two after it. */
    const cli_char *expected_source_revision;
    const cli_char *expected_output_revision;
    const cli_char *expected_metadata_revision;

    /** `--cancel-file`, as given. */
    const cli_char *cancel_file;

    /** `--identity-only`. */
    int identity_only;
} parsed_arguments;

/**
 * Reads the flags from `argv[first]` on into `options`. 0 when a flag is unknown, given twice or
 * missing its value, or when its number does not read.
 */
static int parse_options(int argc, cli_char **argv, int first, parsed_arguments *options) {
    memset(options, 0, sizeof *options);

    /* Each flag is taken once, and every flag but two only with a value after it. */
    for (int at = first; at < argc; ++at) {
        if (equals(argv[at], "--machine") && !options->machine) {
            options->machine = 1;
        } else if (equals(argv[at], "--protocol") && !options->protocol_seen && at + 1 < argc) {
            options->protocol_seen = unsigned_argument(argv[++at], &options->protocol);

            if (!options->protocol_seen) {
                return 0;
            }
        } else if (equals(argv[at], "--mip") && !options->mip_seen && at + 1 < argc) {
            options->mip_seen = unsigned_argument(argv[++at], &options->mip);

            if (!options->mip_seen) {
                return 0;
            }
        } else if (equals(argv[at], "--input") && options->input == NULL && at + 1 < argc) {
            options->input = argv[++at];
        } else if (equals(argv[at], "--output") && options->output == NULL && at + 1 < argc) {
            options->output = argv[++at];
        } else if (equals(argv[at], "--target-format") && options->target_format == NULL && at + 1 < argc) {
            options->target_format = argv[++at];
        } else if (equals(argv[at], "--format-compress") && options->format_compress == NULL && at + 1 < argc) {
            options->format_compress = argv[++at];
        } else if (equals(argv[at], "--compress-threshold") && !options->threshold_seen && at + 1 < argc) {
            options->threshold_seen = unsigned_argument(argv[++at], &options->compress_threshold);

            if (!options->threshold_seen) {
                return 0;
            }
        } else if (equals(argv[at], "--remove-mips") && !options->remove_mips_seen && at + 1 < argc) {
            options->remove_mips_seen = unsigned_argument(argv[++at], &options->remove_mips);

            if (!options->remove_mips_seen) {
                return 0;
            }
        } else if (equals(argv[at], "--conversion") && options->conversion == NULL && at + 1 < argc) {
            options->conversion = argv[++at];
        } else if (equals(argv[at], "--conversion-quality") && !options->quality_seen && at + 1 < argc) {
            options->quality_seen = quality_argument(argv[++at], &options->conversion_quality);

            if (!options->quality_seen) {
                return 0;
            }
        } else if (equals(argv[at], "--swizzling") && options->swizzling == NULL && at + 1 < argc) {
            options->swizzling = argv[++at];
        } else if (equals(argv[at], "--contains-mips") && options->contains_mips == NULL && at + 1 < argc) {
            options->contains_mips = argv[++at];
        } else if (equals(argv[at], "--generate-mips") && options->generate_mips == NULL && at + 1 < argc) {
            options->generate_mips = argv[++at];
        } else if (equals(argv[at], "--normalize") && options->normalize == NULL && at + 1 < argc) {
            options->normalize = argv[++at];
        } else if (equals(argv[at], "--mipmap-function") && options->mipmap_function == NULL && at + 1 < argc) {
            options->mipmap_function = argv[++at];
        } else if (equals(argv[at], "--mipmap-filter") && options->mipmap_filter == NULL && at + 1 < argc) {
            options->mipmap_filter = argv[++at];
        } else if (equals(argv[at], "--tiled-texture") && options->tiled_texture == NULL && at + 1 < argc) {
            options->tiled_texture = argv[++at];
        } else if (equals(argv[at], "--metadata") && options->metadata == NULL && at + 1 < argc) {
            options->metadata = argv[++at];
        } else if (equals(argv[at], "--resource-name") && options->resource_name == NULL && at + 1 < argc) {
            options->resource_name = argv[++at];
        } else if (equals(argv[at], "--source-file") && options->source_file == NULL && at + 1 < argc) {
            options->source_file = argv[++at];
        } else if (equals(argv[at], "--guid") && options->guid == NULL && at + 1 < argc) {
            options->guid = argv[++at];
        } else if (equals(argv[at], "--expect-source-revision") && options->expected_source_revision == NULL && at + 1 < argc) {
            options->expected_source_revision = argv[++at];
        } else if (equals(argv[at], "--expect-output-revision") && options->expected_output_revision == NULL && at + 1 < argc) {
            options->expected_output_revision = argv[++at];
        } else if (equals(argv[at], "--expect-metadata-revision") && options->expected_metadata_revision == NULL && at + 1 < argc) {
            options->expected_metadata_revision = argv[++at];
        } else if (equals(argv[at], "--identity-only") && !options->identity_only) {
            options->identity_only = 1;
        } else if (equals(argv[at], "--cancel-file") && options->cancel_file == NULL && at + 1 < argc) {
            options->cancel_file = argv[++at];
        } else {
            return 0;
        }
    }

    return 1;
}

/** Whether `--output` or any profile flag is given: the flags only `convert` takes. */
static int has_profile_options(const parsed_arguments *options) {
    return options->output != NULL ||
        options->target_format != NULL ||
        options->format_compress != NULL ||
        options->threshold_seen ||
        options->remove_mips_seen ||
        options->conversion != NULL ||
        options->quality_seen ||
        options->swizzling != NULL ||
        options->contains_mips != NULL ||
        options->generate_mips != NULL ||
        options->normalize != NULL ||
        options->mipmap_function != NULL ||
        options->mipmap_filter != NULL ||
        options->tiled_texture != NULL;
}

/** Whether any part of the identity is given: `--resource-name`, `--source-file` or `--guid`. */
static int has_metadata_identity(const parsed_arguments *options) {
    return options->resource_name != NULL || options->source_file != NULL || options->guid != NULL;
}

/** Whether any of the three `--expect-...-revision` flags is given. */
static int has_expected_revisions(const parsed_arguments *options) {
    return options->expected_source_revision != NULL ||
        options->expected_output_revision != NULL ||
        options->expected_metadata_revision != NULL;
}

/**
 * The profile the flags ask for: the default one, with the value of each flag given put in its
 * place. Returns EDDS_OK, or an unsupported-setting refusal for a value the converter does not
 * take.
 */
static edds_status profile_of(const parsed_arguments *options, edds_profile *profile, edds_error *error) {
    edds_default_profile(profile);

    /* Flag by flag: a value that is not one the flag takes is refused. */
    if (options->target_format != NULL && !equals(options->target_format, "enfusion-dds")) {
        goto unsupported;
    }

    if (options->format_compress != NULL) {
        if (equals(options->format_compress, "copy")) {
            profile->format_compress = EDDS_COMPRESS_COPY;
        } else if (equals(options->format_compress, "fastest")) {
            profile->format_compress = EDDS_COMPRESS_FASTEST;
        } else if (equals(options->format_compress, "medium")) {
            profile->format_compress = EDDS_COMPRESS_MEDIUM;
        } else if (equals(options->format_compress, "best")) {
            profile->format_compress = EDDS_COMPRESS_BEST;
        } else {
            goto unsupported;
        }
    }

    /* The compress threshold is at most 100. */
    if (options->threshold_seen) {
        if (options->compress_threshold > 100u) {
            goto unsupported;
        }

        profile->compress_threshold = options->compress_threshold;
    }

    if (options->remove_mips_seen) {
        profile->remove_mips = options->remove_mips;
    }

    /* The conversion is looked up by its wire name among the conversions there are. */
    if (options->conversion != NULL) {
        size_t count = 0;

        const edds_conversion_capability *capabilities = edds_conversions(&count);
        const edds_conversion_capability *chosen       = NULL;

        for (size_t at = 0; at < count; ++at) {
            if (equals(options->conversion, capabilities[at].wire_name)) {
                chosen = &capabilities[at];
            }
        }

        if (chosen == NULL) {
            goto unsupported;
        }

        profile->conversion = chosen->conversion;
    }

    if (options->quality_seen) {
        profile->conversion_quality = options->conversion_quality;
    }

    /* The swizzling the same way, among the swizzlings there are. */
    if (options->swizzling != NULL) {
        size_t count = 0;

        const edds_swizzle_capability *capabilities = edds_swizzles(&count);
        const edds_swizzle_capability *chosen       = NULL;

        for (size_t at = 0; at < count; ++at) {
            if (equals(options->swizzling, capabilities[at].wire_name)) {
                chosen = &capabilities[at];
            }
        }

        if (chosen == NULL) {
            goto unsupported;
        }

        profile->swizzling = chosen->swizzling;
    }

    if (options->contains_mips != NULL) {
        if (equals(options->contains_mips, "true")) {
            profile->contains_mips = 1;
        } else if (equals(options->contains_mips, "false")) {
            profile->contains_mips = 0;
        } else {
            goto unsupported;
        }
    }

    if (options->generate_mips != NULL) {
        if (equals(options->generate_mips, "true")) {
            profile->generate_mips = 1;
        } else if (equals(options->generate_mips, "false")) {
            profile->generate_mips = 0;
        } else {
            goto unsupported;
        }
    }

    if (options->normalize != NULL) {
        if (equals(options->normalize, "true")) {
            profile->normalize = 1;
        } else if (equals(options->normalize, "false")) {
            profile->normalize = 0;
        } else {
            goto unsupported;
        }
    }

    if (options->mipmap_function != NULL) {
        if (equals(options->mipmap_function, "filter")) {
            profile->mipmap_function = EDDS_MIPMAP_FILTER;
        } else if (equals(options->mipmap_function, "normalize")) {
            profile->mipmap_function = EDDS_MIPMAP_NORMALIZE;
        } else if (equals(options->mipmap_function, "color-noise")) {
            profile->mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
        } else {
            goto unsupported;
        }
    }

    if (options->mipmap_filter != NULL) {
        if (equals(options->mipmap_filter, "box")) {
            profile->mipmap_filter = EDDS_FILTER_BOX;
        } else if (equals(options->mipmap_filter, "kaiser")) {
            profile->mipmap_filter = EDDS_FILTER_KAISER;
        } else if (equals(options->mipmap_filter, "triangle")) {
            profile->mipmap_filter = EDDS_FILTER_TRIANGLE;
        } else {
            goto unsupported;
        }
    }

    if (options->tiled_texture != NULL) {
        if (equals(options->tiled_texture, "true")) {
            profile->tiled_texture = 1;
        } else if (equals(options->tiled_texture, "false")) {
            profile->tiled_texture = 0;
        } else {
            goto unsupported;
        }
    }

    /* The conversion and its quality are one combination, so they are judged as one. */
    return edds_profile_check(profile, error);

unsupported:
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "unsupported-setting");
    (void)snprintf(error->message, sizeof error->message,
        "A requested Workbench profile value is recognized but not supported by this conversion slice.");
    return EDDS_UNSUPPORTED_FORMAT;
}

/**
 * When the caller gave the `--expect-...-revision` flags: whether the source, the output and the
 * metadata are still the revisions it planned the conversion with. Returns EDDS_OK, or a
 * `stale-...` refusal that names the first one that is not.
 */
static edds_status validate_revisions(const parsed_arguments *options, edds_error *error) {
    /* Where the metadata is when `--metadata` names none: the output path plus `.meta`. */
    cli_char *metadata_path;

    /* What a file is now, and what the caller planned with. */
    file_revision actual;
    file_revision expected;

    /* The three files, the revision expected of each, and their names for the refusal. */
    const cli_char *paths[3];
    const cli_char *values[3];
    const char     *names[3] = { "source", "output", "metadata" };

    if (!has_expected_revisions(options)) {
        return EDDS_OK;
    }

    metadata_path = append_suffix(options->output, ".meta");

    if (metadata_path == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "allocation-failed");
        (void)snprintf(error->message, sizeof error->message, "Memory for the revision check could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }

    paths[0]  = options->input;
    paths[1]  = options->output;
    paths[2]  = options->metadata != NULL ? options->metadata : metadata_path;
    values[0] = options->expected_source_revision;
    values[1] = options->expected_output_revision;
    values[2] = options->expected_metadata_revision;

    /* Each file as it is now, against the revision the caller expects of it. */
    for (unsigned at = 0; at < 3u; ++at) {
        if (!parse_revision(values[at], &expected) || !revision_of(paths[at], &actual) || !same_revision(&expected, &actual)) {
            free(metadata_path);
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "stale-%s", names[at]);
            (void)snprintf(error->message, sizeof error->message,
                "The %s changed after this conversion was planned; no destination was replaced.",
                names[at]);
            return EDDS_INVALID_INPUT;
        }
    }

    free(metadata_path);

    return EDDS_OK;
}

/** Copies `source` into `destination` when it fits `capacity` with its terminator: 1, or 0. */
static int metadata_text(char *destination, size_t capacity, const char *source) {
    const size_t size = strlen(source);

    if (size >= capacity) {
        return 0;
    }

    memcpy(destination, source, size + 1u);

    return 1;
}

/**
 * Appends to a refusal message and hands back the new end. A truncating or failing `snprintf`
 * parks the cursor at the end of the buffer rather than walking past it, so the message is short
 * rather than wrong.
 */
static size_t append_message(edds_error *error, size_t at, const char *format, ...) {
    va_list arguments;
    int     written;

    if (at >= sizeof error->message) {
        return sizeof error->message;
    }

    va_start(arguments, format);
    written = vsnprintf(error->message + at, sizeof error->message - at, format, arguments);
    va_end(arguments);

    if (written < 0 || (size_t)written >= sizeof error->message - at) {
        return sizeof error->message;
    }

    return at + (size_t)written;
}

/**
 * The source format the path's extension names. Returns EDDS_OK with it in `format`, or an
 * unsupported-format refusal that lists the extensions there are.
 */
static edds_status source_format_of(const cli_char *path, edds_source_format *format, edds_error *error) {
    size_t count = 0;

    /* The source formats there are, each with its one extension. */
    const edds_source_capability *capabilities = edds_source_capabilities(&count);

    /* How much of the refusal message is written so far. */
    size_t written = 0;

    for (size_t at = 0; at < count; ++at) {
        if (ends_with(path, capabilities[at].extension)) {
            *format = capabilities[at].format;
            return EDDS_OK;
        }
    }

    /*
     * The message names the contract rather than a remembered list, so an extension that merely
     * looks like a supported one (`.jpeg` for `.jpg`, `.tif` for `.tiff`) is refused against the
     * same set the editor and the metadata writer use.
     */
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "unsupported-source-extension");
    written = append_message(error, 0, "Only");

    for (size_t at = 0; at < count; ++at) {
        written = append_message(error, written, "%s %s", at == 0 ? "" : (at + 1u == count ? " and" : ","), capabilities[at].extension);
    }

    (void)append_message(error, written, " source paths are supported.");

    return EDDS_UNSUPPORTED_FORMAT;
}

/**
 * The metadata to write: the identity the flags give, the source's format and the profile.
 * Returns EDDS_OK, or a refusal when a part of the identity does not convert or does not fit.
 */
static edds_status metadata_value_of(
    const parsed_arguments *options,
    edds_source_format      format,
    const edds_profile     *profile,
    edds_metadata          *metadata,
    edds_error             *error) {
    char       *name   = utf8_of(options->resource_name);
    char       *source = utf8_of(options->source_file);
    char       *guid   = utf8_of(options->guid);
    edds_status status = EDDS_OK;

    memset(metadata, 0, sizeof *metadata);

    /* The identity, as UTF-8 that fits the metadata's fields. */
    if (name == NULL ||
        source == NULL ||
        guid == NULL ||
        !metadata_text(metadata->name, sizeof metadata->name, name) ||
        !metadata_text(metadata->source_file, sizeof metadata->source_file, source) ||
        !metadata_text(metadata->guid, sizeof metadata->guid, guid)) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "invalid-metadata-identity");
        (void)snprintf(error->message, sizeof error->message, "Resource name, source file, and GUID must fit the native metadata schema.");
        status = EDDS_INVALID_INPUT;
    }

    metadata->source_format = format;
    metadata->profile       = *profile;

    free(name);
    free(source);
    free(guid);

    return status;
}

/**
 * The metadata already at `--metadata`, if there is one, checked against its replacement: the
 * GUID has to stay exactly the same. Returns EDDS_OK, or the refusal.
 */
static edds_status validate_previous_metadata(
    const parsed_arguments *options,
    const edds_metadata    *replacement,
    edds_error             *error) {
    FILE         *input;
    edds_metadata previous;
    edds_status   status;

    if (!path_exists(options->metadata)) {
        return EDDS_OK;
    }

    input = open_input(options->metadata);

    if (input == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "metadata-open-failed");
        (void)snprintf(error->message, sizeof error->message, "Existing metadata could not be opened.");
        return EDDS_INVALID_INPUT;
    }

    status = edds_metadata_parse(input, &previous, error);
    (void)fclose(input);

    /*
     * The replaced recipe is not executed. The parser still validates its complete identity
     * before returning UNSUPPORTED_FORMAT, just as it does for inspect --identity-only.
     */
    if (status != EDDS_OK && status != EDDS_UNSUPPORTED_FORMAT) {
        return status;
    }

    if (strcmp(previous.guid, replacement->guid) != 0) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "metadata-guid-mismatch");
        (void)snprintf(error->message, sizeof error->message,
            "Replacing a registered resource must preserve its existing GUID character-for-character.");
        return EDDS_INVALID_INPUT;
    }

    return EDDS_OK;
}

/**
 * Converts `--input` into `--output`, with its metadata at `--metadata` when that is given, and
 * replaces the destinations together or not at all: each new file is built in a sibling
 * temporary, the old ones are moved aside, and a marker on the disk lets the host recover a pair
 * that a killed process left halfway. Returns EDDS_OK, or the refusal.
 */
static edds_status convert_atomically(
    const parsed_arguments *options,
    const edds_profile     *profile,
    edds_progress_fn        progress,
    void                   *progress_context,
    edds_error             *error) {
    /* The source, and the two temporaries the new files are written to. */
    FILE *source             = NULL;
    FILE *temporary_output   = NULL;
    FILE *temporary_metadata = NULL;

    /* The two destinations, and the marker that publishes them as a pair. */
    cli_artifact    output_artifact   = { options->output, NULL, NULL, 0, 0 };
    cli_artifact    metadata_artifact = { options->metadata, NULL, NULL, 0, 0 };
    cli_transaction transaction       = { NULL, NULL, 0 };

    /* Where the metadata belongs: the output path plus `.meta`. */
    cli_char *expected_metadata = NULL;

    /* The metadata to write, the source's format, and the status. */
    edds_metadata      metadata;
    edds_source_format format;
    edds_status        status;

    /* Whether there is metadata to write, and which old files have been moved aside. */
    int registered      = options->metadata != NULL;
    int output_backed   = 0;
    int metadata_backed = 0;

    if (same_path(options->input, options->output)) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "same-input-output");
        (void)snprintf(error->message, sizeof error->message, "Input and output must be different files.");
        return EDDS_INVALID_INPUT;
    }

    status = source_format_of(options->input, &format, error);

    if (status != EDDS_OK) {
        return status;
    }

    /*
     * The metadata goes right beside the output. Without `--metadata`, no metadata may be there
     * already.
     */
    expected_metadata = append_suffix(options->output, ".meta");

    if (expected_metadata == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "allocation-failed");
        (void)snprintf(error->message, sizeof error->message, "The metadata destination could not be prepared.");
        return EDDS_INTERNAL_FAILURE;
    }

    if (registered && !same_path(options->metadata, expected_metadata)) {
        free(expected_metadata);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "metadata-not-sibling");
        (void)snprintf(error->message, sizeof error->message, "Texture metadata must be the output path plus .meta.");
        return EDDS_INVALID_INPUT;
    }

    if (!registered && path_exists(expected_metadata)) {
        free(expected_metadata);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "detached-metadata-conflict");
        (void)snprintf(error->message, sizeof error->message, "Detached conversion refused because sibling .edds.meta already exists.");
        return EDDS_INVALID_INPUT;
    }

    free(expected_metadata);

    /* The files are still the revisions the caller planned with. */
    status = validate_revisions(options, error);

    if (status != EDDS_OK) {
        return status;
    }

    /* The metadata to write, checked against the metadata it replaces. */
    if (registered) {
        status = metadata_value_of(options, format, profile, &metadata, error);

        if (status != EDDS_OK) {
            return status;
        }

        status = validate_previous_metadata(options, &metadata, error);

        if (status != EDDS_OK) {
            return status;
        }
    }

    /* The new EDDS: the source converted into its temporary, which is flushed and closed. */
    source = open_input(options->input);

    if (source == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "input-open-failed");
        (void)snprintf(error->message, sizeof error->message, "The source image could not be opened (system error %d).", errno);
        return EDDS_INVALID_INPUT;
    }

    temporary_output = create_temporary(&output_artifact, "new");

    if (temporary_output == NULL) {
        (void)fclose(source);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "temporary-open-failed");
        (void)snprintf(error->message, sizeof error->message, "A sibling temporary output could not be created.");
        return EDDS_INTERNAL_FAILURE;
    }

    status = injected("output-write")
        ? injected_failure(error, "output-write")
        : edds_convert(source, format, temporary_output, profile, was_cancelled, NULL,
              progress, progress_context, error);

    if (status == EDDS_OK && !sync_output(temporary_output, "output-flush")) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "temporary-flush-failed");
        (void)snprintf(error->message, sizeof error->message, "The temporary EDDS could not be flushed to disk.");
        status = EDDS_INTERNAL_FAILURE;
    }

    if (fclose(source) != 0 && status == EDDS_OK) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "input-close-failed");
        (void)snprintf(error->message, sizeof error->message, "The source image could not be closed after reading.");
        status = EDDS_INVALID_INPUT;
    }

    if (fclose(temporary_output) != 0 && status == EDDS_OK) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "temporary-close-failed");
        (void)snprintf(error->message, sizeof error->message, "The temporary EDDS could not be closed.");
        status = EDDS_INTERNAL_FAILURE;
    }

    if (status != EDDS_OK) {
        cleanup_artifact(&output_artifact, 0);
        return status;
    }

    /* The new metadata the same way, into a temporary of its own. */
    if (registered) {
        temporary_metadata = create_temporary(&metadata_artifact, "new");

        if (temporary_metadata == NULL) {
            cleanup_artifact(&output_artifact, 0);
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "metadata-temporary-open-failed");
            (void)snprintf(error->message, sizeof error->message, "A sibling temporary metadata file could not be created.");
            return EDDS_INTERNAL_FAILURE;
        }

        status = injected("metadata-write")
            ? injected_failure(error, "metadata-write")
            : edds_metadata_write(temporary_metadata, &metadata, error);

        if (status == EDDS_OK && !sync_output(temporary_metadata, "metadata-flush")) {
            status = injected("metadata-flush") ? injected_failure(error, "metadata-flush") : EDDS_INTERNAL_FAILURE;
        }

        if (fclose(temporary_metadata) != 0 && status == EDDS_OK) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "metadata-temporary-close-failed");
            (void)snprintf(error->message, sizeof error->message, "The temporary metadata file could not be closed.");
            status = EDDS_INTERNAL_FAILURE;
        }

        if (status != EDDS_OK) {
            cleanup_artifact(&output_artifact, 0);
            cleanup_artifact(&metadata_artifact, 0);
            return status;
        }
    }

    /*
     * The last checks before anything is replaced: the files are still the ones planned with, and
     * nobody asked to cancel. Then the pending marker goes on the disk.
     */
    if (injected("cancel-before-commit")) {
        cli_request_cancel();
    }

    status = validate_revisions(options, error);

    if (status == EDDS_OK && was_cancelled(NULL)) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "cancelled");
        (void)snprintf(error->message, sizeof error->message, "The conversion was cancelled before publishing its artifacts.");
        status = EDDS_CANCELLED;
    }

    if (status != EDDS_OK || !begin_transaction(&transaction, options->output)) {
        cleanup_artifact(&output_artifact, 0);
        cleanup_artifact(&metadata_artifact, 0);
        cleanup_transaction(&transaction, 1);

        if (status != EDDS_OK) {
            return status;
        }

        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "transaction-journal-failed");
        (void)snprintf(error->message, sizeof error->message, "The recovery record could not be written; no destination was replaced.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* The old files moved aside: the output first, then the metadata. */
    output_backed = backup_artifact(&output_artifact, "old", "output-backup-rename");
    crash_if_requested("crash-output-backed-up");

    if (output_backed && registered) {
        metadata_backed = backup_artifact(&metadata_artifact, "old", "metadata-backup-rename");
    }

    crash_if_requested("crash-pair-backed-up");

    /* One could not be moved: whatever moved goes back, and once both are back the marker goes. */
    if (!output_backed || (registered && !metadata_backed)) {
        const int metadata_restored = rollback_artifact(&metadata_artifact);
        const int output_restored   = rollback_artifact(&output_artifact);

        cleanup_artifact(&output_artifact, 0);
        cleanup_artifact(&metadata_artifact, 0);
        cleanup_transaction(&transaction, metadata_restored && output_restored);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "artifact-backup-failed");
        (void)snprintf(error->message, sizeof error->message, "The previous EDDS/metadata pair could not be moved aside atomically.");
        return EDDS_INTERNAL_FAILURE;
    }

    /*
     * The new files moved into place, the output first. Then the marker is renamed to committed:
     * the pair's commit point. A failure or a cancel before it puts the old files back.
     */
    {
        int published = commit_artifact(&output_artifact, "output-commit-rename");

        crash_if_requested("crash-output-published");

        if (published && registered) {
            published = commit_artifact(&metadata_artifact, "metadata-commit-rename");
        }

        crash_if_requested("crash-pair-published");
        const int cancelled = was_cancelled(NULL);

        if (published) {
            published = !cancelled && !injected("transaction-commit") && cli_rename(transaction.pending, transaction.committed) == 0;
        }

        if (!published) {
            const int metadata_restored = rollback_artifact(&metadata_artifact);
            const int output_restored   = rollback_artifact(&output_artifact);

            cleanup_artifact(&output_artifact, 0);
            cleanup_artifact(&metadata_artifact, 0);
            cleanup_transaction(&transaction, metadata_restored && output_restored);
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "%s", cancelled ? "cancelled" : "artifact-commit-failed");
            (void)snprintf(error->message, sizeof error->message,
                "The converted EDDS/metadata pair was not committed; its previous artifacts were restored where possible.");
            return cancelled ? EDDS_CANCELLED : EDDS_INTERNAL_FAILURE;
        }
    }

    /* Committed: the temporaries and the backups go, and the marker last. */
    crash_if_requested("crash-transaction-committed");
    cleanup_artifact(&output_artifact, 1);
    crash_if_requested("crash-output-cleaned");
    cleanup_artifact(&metadata_artifact, 1);
    cleanup_transaction(&transaction, 1);

    return EDDS_OK;
}

/**
 * `edds convert`: the profile from the flags, the conversion published, and the new EDDS inspected
 * for the JSON line on stdout. Returns the exit code.
 */
static int convert_command(const parsed_arguments *options) {
    edds_profile profile;
    edds_error   error;
    edds_info    info;
    edds_status  status = profile_of(options, &profile, &error);
    FILE        *output;

    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }

    /* The host asks a running conversion to stop through this file, then waits before a kill. */
    cli_watch_cancel_file(options->cancel_file);
    status = convert_atomically(options, &profile, NULL, NULL, &error);

    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }

    output = open_input(options->output);

    if (output == NULL) {
        memset(&error, 0, sizeof error);
        (void)snprintf(error.code, sizeof error.code, "committed-output-open-failed");
        (void)snprintf(error.message, sizeof error.message, "The committed EDDS could not be inspected.");
        return report_failure(EDDS_INTERNAL_FAILURE, &error);
    }

    /* A committed artifact is a success even if cancellation arrives before reporting it. */
    status = edds_inspect(output, &info, NULL, NULL, &error);
    (void)fclose(output);

    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }

    /* What was written, as one JSON line on stdout. */
    {
        char format_buffer[32];

        (void)printf(
            "{\"protocolVersion\":1,\"kind\":\"convert\",\"width\":%u,\"height\":%u,"
            "\"mipCount\":%u,\"pixelFormat\":\"%s\",\"registered\":%s}\n",
            info.width, info.height, info.mip_count, pixel_format(&info, format_buffer),
            options->metadata != NULL ? "true" : "false");
    }

    return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
}

/** One job of a batch, as a `convert` command line would give it. */
typedef struct native_batch_job {
    /** The id the job record gives. */
    char id[EDDS_BATCH_ID_BYTES];

    /** The job's flags and its profile. */
    parsed_arguments options;
    edds_profile     profile;

    /** The platform strings `options` points to, owned by the job. */
    cli_char *input;
    cli_char *output;
    cli_char *metadata;
    cli_char *resource_name;
    cli_char *source_file;
    cli_char *guid;
    cli_char *expected_source;
    cli_char *expected_output;
    cli_char *expected_metadata;

    /** Whether another job of the batch writes the same output. */
    int collision;
} native_batch_job;

/** Frees the strings a job owns, and leaves the job all zero. */
static void free_batch_job(native_batch_job *job) {
    free(job->input);
    free(job->output);
    free(job->metadata);
    free(job->resource_name);
    free(job->source_file);
    free(job->guid);
    free(job->expected_source);
    free(job->expected_output);
    free(job->expected_metadata);

    memset(job, 0, sizeof *job);
}

/**
 * A job from its batch record: the paths, the identity and the revisions turned into platform
 * strings, and the flags pointed at them. 0 when one of them does not convert; the job is then
 * left empty.
 */
static int native_batch_job_of(const edds_batch_job *source, native_batch_job *job) {
    memset(job, 0, sizeof *job);
    (void)snprintf(job->id, sizeof job->id, "%s", source->id);

    /* The profile as it is, and each string the record has as a platform string. */
    job->profile = source->profile;
    job->input   = cli_of_utf8(source->input);
    job->output  = cli_of_utf8(source->output);

    if (source->has_metadata) {
        job->metadata      = cli_of_utf8(source->metadata);
        job->resource_name = cli_of_utf8(source->resource_name);
        job->source_file   = cli_of_utf8(source->source_file);
        job->guid          = cli_of_utf8(source->guid);
    }

    if (source->has_expected) {
        job->expected_source   = cli_of_utf8(source->expected_source);
        job->expected_output   = cli_of_utf8(source->expected_output);
        job->expected_metadata = cli_of_utf8(source->expected_metadata);
    }

    /* A string the record has that did not convert empties the job again. */
    if (job->input == NULL ||
        job->output == NULL ||
        (source->has_metadata &&
            (job->metadata == NULL ||
                job->resource_name == NULL ||
                job->source_file == NULL ||
                job->guid == NULL)) ||
        (source->has_expected &&
            (job->expected_source == NULL ||
                job->expected_output == NULL ||
                job->expected_metadata == NULL))) {
        free_batch_job(job);
        return 0;
    }

    /* The flags pointed at those strings, the way a command line would set them. */
    job->options.input  = job->input;
    job->options.output = job->output;

    job->options.metadata      = job->metadata;
    job->options.resource_name = job->resource_name;
    job->options.source_file   = job->source_file;
    job->options.guid          = job->guid;

    job->options.expected_source_revision   = job->expected_source;
    job->options.expected_output_revision   = job->expected_output;
    job->options.expected_metadata_revision = job->expected_metadata;

    return 1;
}

/** The batch's stdin: the line framer, and what was read from stdin but not yet framed. */
static struct {
    /** The framer, which hands back whole lines. */
    edds_batch_reader reader;

    /** The last read from stdin, how many bytes it holds, and how many were fed to the framer. */
    char   chunk[8192];
    size_t size;
    size_t at;

    /** Whether stdin has ended. */
    int ended;
} batch_stdin;

/**
 * Stdin arrives in whatever sizes the pipe felt like, so framing is the reader's job and this is
 * only the part that keeps feeding it. 1 is a whole line, 0 is the end of the stream, -1 is a line
 * the protocol refuses.
 */
static int read_batch_line(char *line, size_t capacity, size_t *size, edds_error *error) {
    for (;;) {
        edds_batch_line framed   = EDDS_BATCH_LINE_PENDING;
        size_t          consumed = 0;
        size_t          length   = 0;

        /*
         * The framer gets what is left of the chunk; with nothing left, stdin is read again; once
         * stdin has ended, the framer gives up the last line it holds.
         */
        if (batch_stdin.at < batch_stdin.size) {
            framed = edds_batch_reader_push(&batch_stdin.reader, batch_stdin.chunk + batch_stdin.at,
                batch_stdin.size - batch_stdin.at, &consumed, &length);

            batch_stdin.at += consumed;
        } else if (!batch_stdin.ended) {
            batch_stdin.size = fread(batch_stdin.chunk, 1, sizeof batch_stdin.chunk, stdin);
            batch_stdin.at   = 0;

            if (batch_stdin.size == 0u) {
                batch_stdin.ended = 1;
            }

            continue;
        } else {
            framed = edds_batch_reader_finish(&batch_stdin.reader, &length);

            if (framed == EDDS_BATCH_LINE_PENDING) {
                return 0;
            }
        }

        /* A line over the protocol's limit, or one too long for `line`, is refused. */
        if (framed == EDDS_BATCH_LINE_OVERFLOW || (framed == EDDS_BATCH_LINE_READY && length + 1u > capacity)) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "batch-line-size");
            (void)snprintf(error->message, sizeof error->message, "A batch NDJSON record exceeds the hard line limit.");
            return -1;
        }

        /* A whole line goes to the caller, terminator included. */
        if (framed == EDDS_BATCH_LINE_READY) {
            memcpy(line, batch_stdin.reader.line, length + 1u);
            *size = length;
            return 1;
        }
    }
}

/**
 * Reads the whole batch from stdin: the header with the job count, every job, then the end
 * record and nothing after it. Returns EDDS_OK with `*count` jobs in `*jobs`, which the caller
 * owns, or the refusal. Jobs that would write the same output are marked as colliding.
 */
static edds_status read_batch(native_batch_job **jobs, uint32_t *count, edds_error *error) {
    /* One line of stdin, the record parsed from it, and the jobs loaded so far. */
    char              *line   = malloc(EDDS_BATCH_MAX_LINE_BYTES + 1u);
    edds_batch_record *record = malloc(sizeof *record);
    native_batch_job  *loaded = NULL;

    /* The length of the line, what reading it said, and the status. */
    size_t      size = 0;
    int         read;
    edds_status status;

    if (line == NULL || record == NULL) {
        free(line);
        free(record);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "batch-allocation-failed");
        (void)snprintf(error->message, sizeof error->message, "The batch input buffer could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* The header first: it says how many jobs follow, from 1 to EDDS_BATCH_MAX_JOBS. */
    read = read_batch_line(line, EDDS_BATCH_MAX_LINE_BYTES + 1u, &size, error);

    if (read <= 0) {
        free(line);
        free(record);

        if (read == 0) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "missing-batch-header");
            (void)snprintf(error->message, sizeof error->message, "Batch stdin ended before its header.");
        }

        return EDDS_INVALID_INVOCATION;
    }

    status = edds_batch_parse_line(line, size, record, error);

    if (status != EDDS_OK || record->kind != EDDS_BATCH_HEADER || record->job_count == 0u || record->job_count > EDDS_BATCH_MAX_JOBS) {
        if (status == EDDS_OK) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "batch-job-count");
            (void)snprintf(error->message, sizeof error->message, "A batch must contain between 1 and %u jobs.", EDDS_BATCH_MAX_JOBS);
            status = EDDS_INVALID_INVOCATION;
        }

        free(line);
        free(record);
        return status;
    }

    *count = record->job_count;
    loaded = calloc(*count, sizeof *loaded);

    if (loaded == NULL) {
        free(line);
        free(record);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "batch-allocation-failed");
        (void)snprintf(error->message, sizeof error->message, "The bounded batch job table could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* Then the jobs, one record each, every one with an id of its own. */
    for (uint32_t at = 0; at < *count; ++at) {
        read   = read_batch_line(line, EDDS_BATCH_MAX_LINE_BYTES + 1u, &size, error);
        status = read > 0 ? edds_batch_parse_line(line, size, record, error) : EDDS_INVALID_INVOCATION;

        if (read == 0) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "missing-batch-job");
            (void)snprintf(error->message, sizeof error->message, "Batch stdin ended before every declared job.");
        }

        if (read < 0 || status != EDDS_OK || record->kind != EDDS_BATCH_JOB || !native_batch_job_of(&record->job, &loaded[at])) {
            if (status == EDDS_OK && read > 0) {
                const int early = record->kind != EDDS_BATCH_JOB;

                memset(error, 0, sizeof *error);
                (void)snprintf(error->code, sizeof error->code, "%s", early ? "missing-batch-job" : "invalid-batch-job");
                (void)snprintf(error->message, sizeof error->message, "%s",
                    early ? "The batch ended or restarted before every declared job."
                          : "A batch job path or identity is not valid UTF-8 for this platform.");
                status = EDDS_INVALID_INVOCATION;
            }

            for (uint32_t free_at = 0; free_at <= at; ++free_at) {
                free_batch_job(&loaded[free_at]);
            }

            free(loaded);
            free(line);
            free(record);
            return read < 0 ? EDDS_INVALID_INVOCATION : status;
        }

        for (uint32_t before = 0; before < at; ++before) {
            if (strcmp(loaded[before].id, loaded[at].id) == 0) {
                memset(error, 0, sizeof *error);
                (void)snprintf(error->code, sizeof error->code, "duplicate-batch-id");
                (void)snprintf(error->message, sizeof error->message, "Every batch job id must be unique.");

                for (uint32_t free_at = 0; free_at <= at; ++free_at) {
                    free_batch_job(&loaded[free_at]);
                }

                free(loaded);
                free(line);
                free(record);
                return EDDS_INVALID_INVOCATION;
            }
        }
    }

    /* Then the end record, and nothing after it. */
    read   = read_batch_line(line, EDDS_BATCH_MAX_LINE_BYTES + 1u, &size, error);
    status = read > 0 ? edds_batch_parse_line(line, size, record, error) : EDDS_INVALID_INVOCATION;

    if (read <= 0 ||
        status != EDDS_OK ||
        record->kind != EDDS_BATCH_END ||
        read_batch_line(line, EDDS_BATCH_MAX_LINE_BYTES + 1u, &size, error) != 0) {
        if (status == EDDS_OK) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "invalid-batch-end");
            (void)snprintf(error->message, sizeof error->message, "Batch stdin must end after one end record.");
            status = EDDS_INVALID_INVOCATION;
        }

        for (uint32_t at = 0; at < *count; ++at) {
            free_batch_job(&loaded[at]);
        }

        free(loaded);
        free(line);
        free(record);
        return read < 0 ? EDDS_INVALID_INVOCATION : status;
    }

    /* Normalized once each, so comparing every destination against every other stays cheap. */
    {
        cli_char **destinations = calloc(*count, sizeof *destinations);

        for (uint32_t at = 0; destinations != NULL && at < *count; ++at) {
            destinations[at] = canonical_path(loaded[at].output);
        }

        /* Every pair of jobs; where a normal form is missing, the two paths are compared. */
        for (uint32_t at = 0; at < *count; ++at) {
            for (uint32_t other = at + 1u; other < *count; ++other) {
                const int same = destinations == NULL ||
                        destinations[at] == NULL ||
                        destinations[other] == NULL
                    ? same_path(loaded[at].output, loaded[other].output)
                    : cli_strcmp(destinations[at], destinations[other]) == 0;

                if (same) {
                    loaded[at].collision    = 1;
                    loaded[other].collision = 1;
                }
            }
        }

        for (uint32_t at = 0; destinations != NULL && at < *count; ++at) {
            free(destinations[at]);
        }

        free(destinations);
    }

    free(line);
    free(record);
    *jobs = loaded;

    return EDDS_OK;
}

/** A job's `progress` JSON line on stdout: how far it has got, from 0 to 1. */
static void write_batch_progress(const char *id, double progress) {
    fputs("{\"protocolVersion\":1,\"kind\":\"progress\",\"id\":", stdout);
    json_string(id);
    (void)printf(",\"progress\":%.3f}\n", progress);
}

/**
 * A job's failure as a line for a human on stderr, and as a `diagnostic` JSON line on stdout
 * with a stand-in for a missing code or message.
 */
static void write_batch_diagnostic(const char *id, edds_status status, const edds_error *error) {
    (void)fprintf(stderr, "enfusion: %s: %s\n", edds_status_category(status), error->message);

    fputs("{\"protocolVersion\":1,\"kind\":\"diagnostic\",\"id\":", stdout);
    json_string(id);
    fputs(",\"category\":", stdout);
    json_string(edds_status_category(status));
    fputs(",\"code\":", stdout);
    json_string(error->code[0] == '\0' ? "unspecified" : error->code);
    fputs(",\"message\":", stdout);
    json_string(error->message[0] == '\0' ? "The batch job failed." : error->message);
    fputs("}\n", stdout);
}

/**
 * A job's `result` JSON line on stdout when it did not convert: `status` (`Failed` or
 * `Cancelled`), the reason, and whether it may be retried.
 */
static void write_batch_failed(const char *id, const char *status, const edds_error *error, int retryable) {
    fputs("{\"protocolVersion\":1,\"kind\":\"result\",\"id\":", stdout);
    json_string(id);
    fputs(",\"status\":", stdout);
    json_string(status);
    fputs(",\"reason\":", stdout);
    json_string(error->message[0] == '\0' ? "The batch job did not complete." : error->message);
    (void)printf(",\"retryable\":%s}\n", retryable ? "true" : "false");
}

/** Inspects a job's committed output: EDDS_OK with what it found in `info`, or the refusal. */
static edds_status inspect_converted(const native_batch_job *job, edds_info *info, edds_error *error) {
    FILE       *output = open_input(job->options.output);
    edds_status status;

    if (output == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "committed-output-open-failed");
        (void)snprintf(error->message, sizeof error->message, "The committed EDDS could not be inspected.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* Cancellation may stop later jobs, but cannot relabel an already committed output. */
    status = edds_inspect(output, info, NULL, NULL, error);
    (void)fclose(output);

    return status;
}

/** For a test that asks for it: the cancel file is created once the first job has converted. */
static void inject_batch_cancel_after_first_commit(uint32_t at, edds_status status) {
    if (at != 0u || status != EDDS_OK || !injected("batch-cancel-after-first-commit")) {
        return;
    }

    cli_cancel_through_file();
}

/** A job's `result` JSON line on stdout once it converted: the size, format and mips written. */
static void write_batch_converted(const native_batch_job *job, const edds_info *info) {
    char format_buffer[32];

    fputs("{\"protocolVersion\":1,\"kind\":\"result\",\"id\":", stdout);
    json_string(job->id);
    (void)printf(",\"status\":\"Converted\",\"conversion\":{\"width\":%u,\"height\":%u,"
                 "\"mipCount\":%u,\"pixelFormat\":\"%s\",\"registered\":%s}}\n",
        info->width, info->height, info->mip_count, pixel_format(info, format_buffer),
        job->options.metadata != NULL ? "true" : "false");
}

/** A batch on its way: its jobs, and how many of them converted, failed or were cancelled. */
typedef struct batch_run {
    native_batch_job *jobs;
    uint32_t          converted;
    uint32_t          failed;
    uint32_t          cancelled;
} batch_run;

/** What a job's progress callback works with. */
typedef struct batch_reporter {
    /** The job whose progress is reported. */
    const native_batch_job *job;

    /** The pool, under whose lock stdout is written. */
    edds_pool *pool;

    /** The last progress written. */
    double reported;
} batch_reporter;

/**
 * One row moving while its image converts. Steps below a twentieth are dropped, so a batch of two
 * hundred images cannot flood the one stdout every worker shares.
 */
static void batch_progress(void *context, double progress) {
    batch_reporter *reporter = (batch_reporter *)context;

    /* 1 itself is not written here: the job writes it with its result. */
    if (progress < reporter->reported + 0.05 || progress >= 1.0) {
        return;
    }

    reporter->reported = progress;

    edds_pool_lock_output(reporter->pool);
    write_batch_progress(reporter->job->id, progress);
    (void)fflush(stdout);
    edds_pool_unlock_output(reporter->pool);
}

/** One attempt at a job, as the pool runs it: its conversion, reporting progress as it goes. */
static edds_status convert_batch_attempt(void *context, edds_error *error) {
    batch_reporter *reporter = context;

    return convert_atomically(&reporter->job->options, &reporter->job->profile, batch_progress, reporter, error);
}

/** One image, on whichever worker claimed it. Everything shared is touched under a pool lock. */
static void batch_job_task(void *context, uint32_t at, edds_pool *pool) {
    /* The batch, this job, and the reporter of its progress. */
    batch_run        *run      = (batch_run *)context;
    native_batch_job *job      = &run->jobs[at];
    batch_reporter    reporter = { NULL, NULL, 0.0 };

    /* The source as it is now, the outcome, what the output turned out to be, the charge. */
    file_revision source_revision;
    edds_error    error;
    edds_info     info;
    edds_status   status;
    uint64_t      charge;

    reporter.job  = job;
    reporter.pool = pool;

    /* A failure path that names no error still prints one: an empty one, never stack bytes. */
    memset(&error, 0, sizeof error);

    /* A batch cancelled already: the job is reported as cancelled without being started. */
    if (was_cancelled(NULL)) {
        memset(&error, 0, sizeof error);
        (void)snprintf(error.code, sizeof error.code, "cancelled");
        (void)snprintf(error.message, sizeof error.message, "The batch was cancelled before this job started.");

        edds_pool_lock_output(pool);
        write_batch_failed(job->id, "Cancelled", &error, 0);
        ++run->cancelled;
        (void)fflush(stdout);
        edds_pool_unlock_output(pool);
        return;
    }

    /* A job whose output another job writes as well fails without being started. */
    if (job->collision) {
        memset(&error, 0, sizeof error);
        (void)snprintf(error.code, sizeof error.code, "output-collision");
        (void)snprintf(error.message, sizeof error.message, "Multiple batch jobs resolve to the same output; no winner was selected.");

        edds_pool_lock_output(pool);
        write_batch_diagnostic(job->id, EDDS_INVALID_INPUT, &error);
        write_batch_failed(job->id, "Failed", &error, 0);
        ++run->failed;
        (void)fflush(stdout);
        edds_pool_unlock_output(pool);
        return;
    }

    /* Progress 0: the job has started. */
    edds_pool_lock_output(pool);
    write_batch_progress(job->id, 0.0);
    (void)fflush(stdout);
    edds_pool_unlock_output(pool);

    /* File size is only the initial quota; allocations enforce it even for compressed sources. */
    charge = edds_pool_charge_of(revision_of(job->options.input, &source_revision) && source_revision.exists ? source_revision.size : 0u);
    status = edds_pool_execute(pool, charge, convert_batch_attempt, &reporter, was_cancelled, NULL, &error);
    inject_batch_cancel_after_first_commit(at, status);

    if (status == EDDS_OK) {
        status = inspect_converted(job, &info, &error);
    }

    /* The result, written whole under the lock, and counted. */
    edds_pool_lock_output(pool);

    if (status == EDDS_OK) {
        write_batch_progress(job->id, 1.0);
        write_batch_converted(job, &info);
        ++run->converted;
    } else if (status == EDDS_CANCELLED || was_cancelled(NULL)) {
        write_batch_diagnostic(job->id, EDDS_CANCELLED, &error);
        write_batch_failed(job->id, "Cancelled", &error, 0);
        ++run->cancelled;
    } else {
        write_batch_diagnostic(job->id, status, &error);
        write_batch_failed(job->id, "Failed", &error, status == EDDS_INTERNAL_FAILURE);
        ++run->failed;
    }

    (void)fflush(stdout);
    edds_pool_unlock_output(pool);
}

/**
 * `edds batch`: the jobs read from stdin and run over the worker pool, each writing its own
 * lines, then a `complete` line with the counts. Returns the exit code, which is the cancelled
 * status when the batch was cancelled.
 */
static int batch_command(void) {
    batch_run   run   = { NULL, 0, 0, 0 };
    uint32_t    count = 0;
    edds_error  error;
    edds_status status = read_batch(&run.jobs, &count, &error);

    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }

    (void)printf("{\"protocolVersion\":1,\"kind\":\"batch-started\",\"jobCount\":%u}\n", count);
    (void)fflush(stdout);

    status = edds_pool_run(count, EDDS_POOL_MEMORY_BUDGET, batch_job_task, &run, &error);

    if (status != EDDS_OK) {
        for (uint32_t at = 0; at < count; ++at) {
            free_batch_job(&run.jobs[at]);
        }

        free(run.jobs);
        return report_failure(status, &error);
    }

    (void)printf("{\"protocolVersion\":1,\"kind\":\"complete\",\"converted\":%u,"
                 "\"failed\":%u,\"cancelled\":%u}\n",
        run.converted, run.failed, run.cancelled);

    for (uint32_t at = 0; at < count; ++at) {
        free_batch_job(&run.jobs[at]);
    }

    free(run.jobs);

    if (ferror(stdout)) {
        return EDDS_INTERNAL_FAILURE;
    }

    return was_cancelled(NULL) ? EDDS_CANCELLED : 0;
}

/** `enfusion edds ...`; `argv[0]` is the area itself. Returns the exit code. */
int edds_command(int argc, cli_char **argv) {
    /* The command line's flags. */
    parsed_arguments options;

    /* The input file, what inspecting it found, and the error of a failure. */
    FILE      *input;
    edds_info  info;
    edds_error error;

    /* The metadata `--metadata` names, and the one the inspection reports: NULL for none. */
    edds_metadata  metadata;
    edds_metadata *inspected_metadata = NULL;

    /* Why the metadata's recipe is refused, when only its identity is reported. */
    char unsupported_metadata_reason[sizeof error.message] = { 0 };

    edds_status status;

    /* Which command this is: preview, convert, or else inspect. */
    int preview_command;
    int convert;

    /* The preview's RGBA pixels, and how many bytes they take. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    cli_catch_interrupts();

    /* `batch --machine --protocol 1`, and `--cancel-file PATH` or nothing after it. */
    if ((argc == 5 || argc == 7) &&
        equals(argv[1], "batch") &&
        equals(argv[2], "--machine") &&
        equals(argv[3], "--protocol") &&
        equals(argv[4], EDDS_PROTOCOL_TEXT) &&
        (argc == 5 || equals(argv[5], "--cancel-file"))) {
        int result;

        cli_watch_cancel_file(argc == 7 ? argv[6] : NULL);
        result = batch_command();
        cli_watch_cancel_file(NULL);
        return result;
    }

    if (argc < 2 || (!equals(argv[1], "inspect") && !equals(argv[1], "preview") && !equals(argv[1], "convert"))) {
        return invalid_invocation("invalid-command", "Expected inspect, preview, convert, or batch.");
    }

    preview_command = equals(argv[1], "preview");
    convert         = equals(argv[1], "convert");

    /*
     * All three need `--machine --protocol 1 --input`. `--mip` is preview's, which needs it, and
     * preview takes no `--metadata`. Only convert takes `--output` (which it needs), the
     * profile flags, the identity, the expected revisions and `--cancel-file`; its `--metadata`
     * comes with the whole identity or neither does, and the revisions come all three or none.
     * `--identity-only` is for inspect with `--metadata`.
     */
    if (!parse_options(argc, argv, 2, &options) ||
        !options.machine ||
        !options.protocol_seen ||
        options.protocol != EDDS_PROTOCOL_VERSION ||
        options.input == NULL ||
        (preview_command && (!options.mip_seen || has_profile_options(&options))) ||
        (!preview_command && options.mip_seen) ||
        (convert &&
            (options.output == NULL ||
                ((options.metadata == NULL) != !has_metadata_identity(&options)) ||
                (options.metadata != NULL &&
                    (options.resource_name == NULL ||
                        options.source_file == NULL ||
                        options.guid == NULL)))) ||
        (has_expected_revisions(&options) &&
            (options.expected_source_revision == NULL ||
                options.expected_output_revision == NULL ||
                options.expected_metadata_revision == NULL)) ||
        (!convert && has_expected_revisions(&options)) ||
        (!convert && (has_profile_options(&options) || has_metadata_identity(&options))) ||
        (preview_command && options.metadata != NULL) ||
        (!convert && options.cancel_file != NULL) ||
        (options.identity_only && (preview_command || convert || options.metadata == NULL))) {
        return invalid_invocation("invalid-options", "The command options are incomplete, duplicated, or unsupported.");
    }

    if (convert) {
        return convert_command(&options);
    }

    /* Inspect and preview both begin by inspecting the input. */
    input = open_input(options.input);

    if (input == NULL) {
        memset(&error, 0, sizeof error);
        (void)snprintf(error.code, sizeof error.code, "input-open-failed");
        (void)snprintf(error.message, sizeof error.message, "The input file could not be opened (system error %d).", errno);
        return report_failure(EDDS_INVALID_INPUT, &error);
    }

    status = edds_inspect(input, &info, was_cancelled, NULL, &error);

    if (status != EDDS_OK) {
        (void)fclose(input);
        return report_failure(status, &error);
    }

    /* Inspect: what the file is, and its metadata when `--metadata` names it. */
    if (!preview_command) {
        if (options.metadata != NULL) {
            FILE *metadata_input = open_input(options.metadata);

            if (metadata_input == NULL) {
                (void)fclose(input);
                memset(&error, 0, sizeof error);
                (void)snprintf(error.code, sizeof error.code, "metadata-open-failed");
                (void)snprintf(error.message, sizeof error.message, "The explicit metadata file could not be opened.");
                return report_failure(EDDS_INVALID_INPUT, &error);
            }

            status = edds_metadata_parse(metadata_input, &metadata, &error);
            (void)fclose(metadata_input);

            /*
             * With `--identity-only`, a recipe the converter does not support is no failure: its
             * identity is still reported, with the parser's message as the reason.
             */
            if (status == EDDS_UNSUPPORTED_FORMAT && options.identity_only) {
                (void)snprintf(unsupported_metadata_reason, sizeof unsupported_metadata_reason, "%s", error.message);
                status = EDDS_OK;
            }

            if (status != EDDS_OK) {
                (void)fclose(input);
                return report_failure(status, &error);
            }

            inspected_metadata = &metadata;
        }

        write_inspection(&info, inspected_metadata, unsupported_metadata_reason[0] == '\0' ? NULL : unsupported_metadata_reason);
        (void)fclose(input);

        return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
    }

    /* Preview: the `--mip` level decoded to RGBA. */
    status = edds_preview(input, &info, options.mip, was_cancelled, NULL, &rgba, &rgba_size, &error);
    (void)fclose(input);

    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }

    write_preview(&info, options.mip, rgba, rgba_size);
    edds_free(rgba);

    return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
}
