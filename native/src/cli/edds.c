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

enum { EDDS_PROTOCOL_VERSION = 1 };

#define EDDS_PROTOCOL_TEXT "1"

/**
 * `--conversion-quality` on the command line, in thousandths. The CLI takes the same text the
 * Workbench recipe holds — `1`, `0.5`, `0.403` — so a profile can be handed straight across.
 */
static int quality_argument(const cli_char *text, uint32_t *value) {
    uint32_t whole = 0;
    uint32_t fraction = 0;
    unsigned digits = 0;
    int seen = 0;
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
    if (*text != 0) {
        return 0;
    }
    while (digits < 3u) {
        fraction *= 10u;
        ++digits;
    }
    *value = whole * EDDS_QUALITY_SCALE + fraction;
    return *value <= EDDS_QUALITY_SCALE;
}

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

static int invalid_invocation(const char *code, const char *message) {
    edds_error error;
    memset(&error, 0, sizeof error);
    (void)snprintf(error.code, sizeof error.code, "%s", code);
    (void)snprintf(error.message, sizeof error.message, "%s", message);
    usage();
    return report_failure(EDDS_INVALID_INVOCATION, &error);
}

static const char *pixel_format(const edds_info *info, char buffer[32]) {
    if (info->pixel_format == EDDS_PIXEL_DXGI) {
        (void)snprintf(buffer, 32, "DXGI_%u", info->dxgi_format);
        return buffer;
    }
    return edds_pixel_format_name(info->pixel_format);
}

static const char *preview_refusal(const edds_info *info) {
    if (info->pixel_format == EDDS_PIXEL_DXGI || info->pixel_format == EDDS_PIXEL_UNKNOWN) {
        return "Pixel preview is unavailable because this DDS pixel format is not supported.";
    }
    if ((uint64_t)info->width * info->height * 4u > EDDS_MAX_PREVIEW_BYTES) {
        return "Pixel preview is unavailable because this texture decodes to more pixels than one preview holds.";
    }
    return "Pixel preview is unavailable because only one two-dimensional texture surface is supported.";
}

static const char *metadata_compress(edds_format_compress compress) {
    switch (compress) {
        case EDDS_COMPRESS_COPY: return "Copy";
        case EDDS_COMPRESS_FASTEST: return "Fastest";
        case EDDS_COMPRESS_MEDIUM: return "Medium";
        case EDDS_COMPRESS_BEST: return "Best";
        default: return "Unknown";
    }
}

static const char *metadata_mipmap_function(edds_mipmap_function function) {
    switch (function) {
        case EDDS_MIPMAP_FILTER: return "Filter";
        case EDDS_MIPMAP_NORMALIZE: return "Normalize";
        case EDDS_MIPMAP_COLOR_NOISE: return "ColorNoise";
        default: return "Unknown";
    }
}

static const char *metadata_mipmap_filter(edds_mipmap_filter filter) {
    switch (filter) {
        case EDDS_FILTER_BOX: return "Box";
        case EDDS_FILTER_KAISER: return "Kaiser";
        case EDDS_FILTER_TRIANGLE: return "Triangle";
        default: return "Unknown";
    }
}

/** The same shortest exact text the metadata carries, as a JSON number rather than a string. */
static const char *quality_json(uint32_t value, char buffer[8]) {
    const uint32_t whole = value / EDDS_QUALITY_SCALE;
    const uint32_t fraction = value % EDDS_QUALITY_SCALE;
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

static void write_inspection(
    const edds_info *info,
    const edds_metadata *metadata,
    const char *unsupported_metadata_reason) {
    char format_buffer[32];
    char quality_buffer[8];
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
            const edds_source_capability *capability =
                edds_source_capability_of_format(metadata->source_format);
            json_string(capability == NULL ? "" : capability->wire_name);
        }
        fputs("},\"recipe\":{\"TargetFormat\":\"EnfusionDDS\",\"FormatCompress\":", stdout);
        json_string(metadata_compress(metadata->profile.format_compress));
        (void)printf(",\"CompressTreshold\":%u,\"RemoveMips\":%u,\"Conversion\":",
            metadata->profile.compress_threshold, metadata->profile.remove_mips);
        {
            const edds_conversion_capability *conversion =
                edds_conversion_capability_of(metadata->profile.conversion);
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
        (void)printf(",\"TiledTexture\":%s}}",
            metadata->profile.tiled_texture ? "true" : "false");
    }
    fputs("}\n", stdout);
}

static void write_base64(const uint8_t *bytes, size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    while (size - at >= 3) {
        const uint32_t value = ((uint32_t)bytes[at] << 16) |
            ((uint32_t)bytes[at + 1] << 8) | bytes[at + 2];
        putchar(alphabet[(value >> 18) & 63u]);
        putchar(alphabet[(value >> 12) & 63u]);
        putchar(alphabet[(value >> 6) & 63u]);
        putchar(alphabet[value & 63u]);
        at += 3;
    }
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

typedef struct parsed_arguments {
    int machine;
    int protocol_seen;
    uint32_t protocol;
    int mip_seen;
    uint32_t mip;
    const cli_char *input;
    const cli_char *output;
    const cli_char *target_format;
    const cli_char *format_compress;
    int threshold_seen;
    uint32_t compress_threshold;
    int remove_mips_seen;
    uint32_t remove_mips;
    const cli_char *conversion;
    int quality_seen;
    uint32_t conversion_quality;
    const cli_char *swizzling;
    const cli_char *contains_mips;
    const cli_char *generate_mips;
    const cli_char *normalize;
    const cli_char *mipmap_function;
    const cli_char *mipmap_filter;
    const cli_char *tiled_texture;
    const cli_char *metadata;
    const cli_char *resource_name;
    const cli_char *source_file;
    const cli_char *guid;
    const cli_char *expected_source_revision;
    const cli_char *expected_output_revision;
    const cli_char *expected_metadata_revision;
    const cli_char *cancel_file;
    int identity_only;
} parsed_arguments;

static int parse_options(int argc, cli_char **argv, int first, parsed_arguments *options) {
    memset(options, 0, sizeof *options);
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
        } else if (equals(argv[at], "--expect-source-revision") &&
            options->expected_source_revision == NULL && at + 1 < argc) {
            options->expected_source_revision = argv[++at];
        } else if (equals(argv[at], "--expect-output-revision") &&
            options->expected_output_revision == NULL && at + 1 < argc) {
            options->expected_output_revision = argv[++at];
        } else if (equals(argv[at], "--expect-metadata-revision") &&
            options->expected_metadata_revision == NULL && at + 1 < argc) {
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

static int has_profile_options(const parsed_arguments *options) {
    return options->output != NULL || options->target_format != NULL ||
        options->format_compress != NULL || options->threshold_seen ||
        options->remove_mips_seen ||
        options->conversion != NULL || options->quality_seen || options->swizzling != NULL ||
        options->contains_mips != NULL || options->generate_mips != NULL ||
        options->normalize != NULL || options->mipmap_function != NULL ||
        options->mipmap_filter != NULL || options->tiled_texture != NULL;
}

static int has_metadata_identity(const parsed_arguments *options) {
    return options->resource_name != NULL || options->source_file != NULL || options->guid != NULL;
}

static int has_expected_revisions(const parsed_arguments *options) {
    return options->expected_source_revision != NULL ||
        options->expected_output_revision != NULL || options->expected_metadata_revision != NULL;
}

static edds_status profile_of(
    const parsed_arguments *options,
    edds_profile *profile,
    edds_error *error) {
    edds_default_profile(profile);
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
    if (options->threshold_seen) {
        if (options->compress_threshold > 100u) {
            goto unsupported;
        }
        profile->compress_threshold = options->compress_threshold;
    }
    if (options->remove_mips_seen) {
        profile->remove_mips = options->remove_mips;
    }
    if (options->conversion != NULL) {
        size_t count = 0;
        const edds_conversion_capability *capabilities = edds_conversions(&count);
        const edds_conversion_capability *chosen = NULL;
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
    if (options->swizzling != NULL) {
        size_t count = 0;
        const edds_swizzle_capability *capabilities = edds_swizzles(&count);
        const edds_swizzle_capability *chosen = NULL;
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

static edds_status validate_revisions(
    const parsed_arguments *options,
    edds_error *error) {
    cli_char *metadata_path;
    file_revision actual;
    file_revision expected;
    const cli_char *paths[3];
    const cli_char *values[3];
    const char *names[3] = { "source", "output", "metadata" };
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
    paths[0] = options->input;
    paths[1] = options->output;
    paths[2] = options->metadata != NULL ? options->metadata : metadata_path;
    values[0] = options->expected_source_revision;
    values[1] = options->expected_output_revision;
    values[2] = options->expected_metadata_revision;
    for (unsigned at = 0; at < 3u; ++at) {
        if (!parse_revision(values[at], &expected) || !revision_of(paths[at], &actual) ||
            !same_revision(&expected, &actual)) {
            free(metadata_path);
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "stale-%s", names[at]);
            (void)snprintf(error->message, sizeof error->message,
                "The %s changed after this conversion was planned; no destination was replaced.", names[at]);
            return EDDS_INVALID_INPUT;
        }
    }
    free(metadata_path);
    return EDDS_OK;
}

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
    int written;
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

static edds_status source_format_of(
    const cli_char *path,
    edds_source_format *format,
    edds_error *error) {
    size_t count = 0;
    const edds_source_capability *capabilities = edds_source_capabilities(&count);
    size_t written = 0;
    for (size_t at = 0; at < count; ++at) {
        if (ends_with(path, capabilities[at].extension)) {
            *format = capabilities[at].format;
            return EDDS_OK;
        }
    }
    /*
     * The message names the contract rather than a remembered list, so an extension that merely
     * looks like a supported one — `.jpeg` for `.jpg`, `.tif` for `.tiff` — is refused against the
     * same set the editor and the metadata writer use.
     */
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "unsupported-source-extension");
    written = append_message(error, 0, "Only");
    for (size_t at = 0; at < count; ++at) {
        written = append_message(error, written, "%s %s",
            at == 0 ? "" : (at + 1u == count ? " and" : ","), capabilities[at].extension);
    }
    (void)append_message(error, written, " source paths are supported.");
    return EDDS_UNSUPPORTED_FORMAT;
}

static edds_status metadata_value_of(
    const parsed_arguments *options,
    edds_source_format format,
    const edds_profile *profile,
    edds_metadata *metadata,
    edds_error *error) {
    char *name = utf8_of(options->resource_name);
    char *source = utf8_of(options->source_file);
    char *guid = utf8_of(options->guid);
    edds_status status = EDDS_OK;
    memset(metadata, 0, sizeof *metadata);
    if (name == NULL || source == NULL || guid == NULL ||
        !metadata_text(metadata->name, sizeof metadata->name, name) ||
        !metadata_text(metadata->source_file, sizeof metadata->source_file, source) ||
        !metadata_text(metadata->guid, sizeof metadata->guid, guid)) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "invalid-metadata-identity");
        (void)snprintf(error->message, sizeof error->message,
            "Resource name, source file, and GUID must fit the native metadata schema.");
        status = EDDS_INVALID_INPUT;
    }
    metadata->source_format = format;
    metadata->profile = *profile;
    free(name);
    free(source);
    free(guid);
    return status;
}

static edds_status validate_previous_metadata(
    const parsed_arguments *options,
    const edds_metadata *replacement,
    edds_error *error) {
    FILE *input;
    edds_metadata previous;
    edds_status status;
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
    /* The replaced recipe is not executed. The parser still validates its complete identity
     * before returning UNSUPPORTED_FORMAT, just as it does for inspect --identity-only. */
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

static edds_status convert_atomically(
    const parsed_arguments *options,
    const edds_profile *profile,
    edds_progress_fn progress,
    void *progress_context,
    edds_error *error) {
    FILE *source = NULL;
    FILE *temporary_output = NULL;
    FILE *temporary_metadata = NULL;
    cli_artifact output_artifact = { options->output, NULL, NULL, 0, 0 };
    cli_artifact metadata_artifact = { options->metadata, NULL, NULL, 0, 0 };
    cli_transaction transaction = { NULL, NULL, 0 };
    cli_char *expected_metadata = NULL;
    edds_metadata metadata;
    edds_source_format format;
    edds_status status;
    int registered = options->metadata != NULL;
    int output_backed = 0;
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
        (void)snprintf(error->message, sizeof error->message,
            "Detached conversion refused because sibling .edds.meta already exists.");
        return EDDS_INVALID_INPUT;
    }
    free(expected_metadata);
    status = validate_revisions(options, error);
    if (status != EDDS_OK) {
        return status;
    }
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
    source = open_input(options->input);
    if (source == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "input-open-failed");
        (void)snprintf(error->message, sizeof error->message,
            "The source image could not be opened (system error %d).", errno);
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
            status = injected("metadata-flush")
                ? injected_failure(error, "metadata-flush")
                : EDDS_INTERNAL_FAILURE;
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
    if (injected("cancel-before-commit")) {
        cli_request_cancel();
    }
    status = validate_revisions(options, error);
    if (status == EDDS_OK && was_cancelled(NULL)) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "cancelled");
        (void)snprintf(error->message, sizeof error->message,
            "The conversion was cancelled before publishing its artifacts.");
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
        (void)snprintf(error->message, sizeof error->message,
            "The recovery record could not be written; no destination was replaced.");
        return EDDS_INTERNAL_FAILURE;
    }
    output_backed = backup_artifact(&output_artifact, "old", "output-backup-rename");
    crash_if_requested("crash-output-backed-up");
    if (output_backed && registered) {
        metadata_backed = backup_artifact(&metadata_artifact, "old", "metadata-backup-rename");
    }
    crash_if_requested("crash-pair-backed-up");
    if (!output_backed || (registered && !metadata_backed)) {
        const int metadata_restored = rollback_artifact(&metadata_artifact);
        const int output_restored = rollback_artifact(&output_artifact);
        cleanup_artifact(&output_artifact, 0);
        cleanup_artifact(&metadata_artifact, 0);
        cleanup_transaction(&transaction, metadata_restored && output_restored);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "artifact-backup-failed");
        (void)snprintf(error->message, sizeof error->message, "The previous EDDS/metadata pair could not be moved aside atomically.");
        return EDDS_INTERNAL_FAILURE;
    }
    {
        int published = commit_artifact(&output_artifact, "output-commit-rename");
        crash_if_requested("crash-output-published");
        if (published && registered) {
            published = commit_artifact(&metadata_artifact, "metadata-commit-rename");
        }
        crash_if_requested("crash-pair-published");
        const int cancelled = was_cancelled(NULL);
        if (published) {
            published = !cancelled && !injected("transaction-commit") &&
                cli_rename(transaction.pending, transaction.committed) == 0;
        }
        if (!published) {
            const int metadata_restored = rollback_artifact(&metadata_artifact);
            const int output_restored = rollback_artifact(&output_artifact);
            cleanup_artifact(&output_artifact, 0);
            cleanup_artifact(&metadata_artifact, 0);
            cleanup_transaction(&transaction, metadata_restored && output_restored);
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code,
                "%s", cancelled ? "cancelled" : "artifact-commit-failed");
            (void)snprintf(error->message, sizeof error->message,
                "The converted EDDS/metadata pair was not committed; its previous artifacts were restored where possible.");
            return cancelled ? EDDS_CANCELLED : EDDS_INTERNAL_FAILURE;
        }
    }
    crash_if_requested("crash-transaction-committed");
    cleanup_artifact(&output_artifact, 1);
    crash_if_requested("crash-output-cleaned");
    cleanup_artifact(&metadata_artifact, 1);
    cleanup_transaction(&transaction, 1);
    return EDDS_OK;
}

static int convert_command(const parsed_arguments *options) {
    edds_profile profile;
    edds_error error;
    edds_info info;
    edds_status status = profile_of(options, &profile, &error);
    FILE *output;
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

typedef struct native_batch_job {
    char id[EDDS_BATCH_ID_BYTES];
    parsed_arguments options;
    edds_profile profile;
    cli_char *input;
    cli_char *output;
    cli_char *metadata;
    cli_char *resource_name;
    cli_char *source_file;
    cli_char *guid;
    cli_char *expected_source;
    cli_char *expected_output;
    cli_char *expected_metadata;
    int collision;
} native_batch_job;

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

static int native_batch_job_of(const edds_batch_job *source, native_batch_job *job) {
    memset(job, 0, sizeof *job);
    (void)snprintf(job->id, sizeof job->id, "%s", source->id);
    job->profile = source->profile;
    job->input = cli_of_utf8(source->input);
    job->output = cli_of_utf8(source->output);
    if (source->has_metadata) {
        job->metadata = cli_of_utf8(source->metadata);
        job->resource_name = cli_of_utf8(source->resource_name);
        job->source_file = cli_of_utf8(source->source_file);
        job->guid = cli_of_utf8(source->guid);
    }
    if (source->has_expected) {
        job->expected_source = cli_of_utf8(source->expected_source);
        job->expected_output = cli_of_utf8(source->expected_output);
        job->expected_metadata = cli_of_utf8(source->expected_metadata);
    }
    if (job->input == NULL || job->output == NULL ||
        (source->has_metadata && (job->metadata == NULL || job->resource_name == NULL || job->source_file == NULL || job->guid == NULL)) ||
        (source->has_expected && (job->expected_source == NULL || job->expected_output == NULL || job->expected_metadata == NULL))) {
        free_batch_job(job);
        return 0;
    }
    job->options.input = job->input;
    job->options.output = job->output;
    job->options.metadata = job->metadata;
    job->options.resource_name = job->resource_name;
    job->options.source_file = job->source_file;
    job->options.guid = job->guid;
    job->options.expected_source_revision = job->expected_source;
    job->options.expected_output_revision = job->expected_output;
    job->options.expected_metadata_revision = job->expected_metadata;
    return 1;
}

/**
 * Stdin arrives in whatever sizes the pipe felt like, so framing is the reader's job and this is
 * only the part that keeps feeding it. 1 is a whole line, 0 is the end of the stream, -1 is a line
 * the protocol refuses.
 */
static struct {
    edds_batch_reader reader;
    char chunk[8192];
    size_t size;
    size_t at;
    int ended;
} batch_stdin;

static int read_batch_line(char *line, size_t capacity, size_t *size, edds_error *error) {
    for (;;) {
        edds_batch_line framed = EDDS_BATCH_LINE_PENDING;
        size_t consumed = 0;
        size_t length = 0;
        if (batch_stdin.at < batch_stdin.size) {
            framed = edds_batch_reader_push(&batch_stdin.reader, batch_stdin.chunk + batch_stdin.at,
                batch_stdin.size - batch_stdin.at, &consumed, &length);
            batch_stdin.at += consumed;
        } else if (!batch_stdin.ended) {
            batch_stdin.size = fread(batch_stdin.chunk, 1, sizeof batch_stdin.chunk, stdin);
            batch_stdin.at = 0;
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
        if (framed == EDDS_BATCH_LINE_OVERFLOW || (framed == EDDS_BATCH_LINE_READY && length + 1u > capacity)) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "batch-line-size");
            (void)snprintf(error->message, sizeof error->message,
                "A batch NDJSON record exceeds the hard line limit.");
            return -1;
        }
        if (framed == EDDS_BATCH_LINE_READY) {
            memcpy(line, batch_stdin.reader.line, length + 1u);
            *size = length;
            return 1;
        }
    }
}

static edds_status read_batch(
    native_batch_job **jobs,
    uint32_t *count,
    edds_error *error) {
    char *line = malloc(EDDS_BATCH_MAX_LINE_BYTES + 1u);
    edds_batch_record *record = malloc(sizeof *record);
    native_batch_job *loaded = NULL;
    size_t size = 0;
    int read;
    edds_status status;
    if (line == NULL || record == NULL) {
        free(line);
        free(record);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "batch-allocation-failed");
        (void)snprintf(error->message, sizeof error->message, "The batch input buffer could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
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
    if (status != EDDS_OK || record->kind != EDDS_BATCH_HEADER || record->job_count == 0u ||
        record->job_count > EDDS_BATCH_MAX_JOBS) {
        if (status == EDDS_OK) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "batch-job-count");
            (void)snprintf(error->message, sizeof error->message,
                "A batch must contain between 1 and %u jobs.", EDDS_BATCH_MAX_JOBS);
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
    for (uint32_t at = 0; at < *count; ++at) {
        read = read_batch_line(line, EDDS_BATCH_MAX_LINE_BYTES + 1u, &size, error);
        status = read > 0 ? edds_batch_parse_line(line, size, record, error) : EDDS_INVALID_INVOCATION;
        if (read == 0) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "missing-batch-job");
            (void)snprintf(error->message, sizeof error->message, "Batch stdin ended before every declared job.");
        }
        if (read < 0 || status != EDDS_OK || record->kind != EDDS_BATCH_JOB ||
            !native_batch_job_of(&record->job, &loaded[at])) {
            if (status == EDDS_OK && read > 0) {
                const int early = record->kind != EDDS_BATCH_JOB;
                memset(error, 0, sizeof *error);
                (void)snprintf(error->code, sizeof error->code, "%s",
                    early ? "missing-batch-job" : "invalid-batch-job");
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
                (void)snprintf(error->message, sizeof error->message,
                    "Every batch job id must be unique.");
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
    read = read_batch_line(line, EDDS_BATCH_MAX_LINE_BYTES + 1u, &size, error);
    status = read > 0 ? edds_batch_parse_line(line, size, record, error) : EDDS_INVALID_INVOCATION;
    if (read <= 0 || status != EDDS_OK || record->kind != EDDS_BATCH_END ||
        read_batch_line(line, EDDS_BATCH_MAX_LINE_BYTES + 1u, &size, error) != 0) {
        if (status == EDDS_OK) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "invalid-batch-end");
            (void)snprintf(error->message, sizeof error->message,
                "Batch stdin must end after one end record.");
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
        for (uint32_t at = 0; at < *count; ++at) {
            for (uint32_t other = at + 1u; other < *count; ++other) {
                const int same = destinations == NULL || destinations[at] == NULL ||
                        destinations[other] == NULL
                    ? same_path(loaded[at].output, loaded[other].output)
                    : cli_strcmp(destinations[at], destinations[other]) == 0;
                if (same) {
                    loaded[at].collision = 1;
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

static void write_batch_progress(const char *id, double progress) {
    fputs("{\"protocolVersion\":1,\"kind\":\"progress\",\"id\":", stdout);
    json_string(id);
    (void)printf(",\"progress\":%.3f}\n", progress);
}

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

static void write_batch_failed(
    const char *id,
    const char *status,
    const edds_error *error,
    int retryable) {
    fputs("{\"protocolVersion\":1,\"kind\":\"result\",\"id\":", stdout);
    json_string(id);
    fputs(",\"status\":", stdout);
    json_string(status);
    fputs(",\"reason\":", stdout);
    json_string(error->message[0] == '\0' ? "The batch job did not complete." : error->message);
    (void)printf(",\"retryable\":%s}\n", retryable ? "true" : "false");
}

static edds_status inspect_converted(const native_batch_job *job, edds_info *info, edds_error *error) {
    FILE *output = open_input(job->options.output);
    edds_status status;
    if (output == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "committed-output-open-failed");
        (void)snprintf(error->message, sizeof error->message,
            "The committed EDDS could not be inspected.");
        return EDDS_INTERNAL_FAILURE;
    }
    /* Cancellation may stop later jobs, but cannot relabel an already committed output. */
    status = edds_inspect(output, info, NULL, NULL, error);
    (void)fclose(output);
    return status;
}

static void inject_batch_cancel_after_first_commit(uint32_t at, edds_status status) {
    if (at != 0u || status != EDDS_OK || !injected("batch-cancel-after-first-commit")) {
        return;
    }
    cli_cancel_through_file();
}

static void write_batch_converted(const native_batch_job *job, const edds_info *info) {
    char format_buffer[32];
    fputs("{\"protocolVersion\":1,\"kind\":\"result\",\"id\":", stdout);
    json_string(job->id);
    (void)printf(",\"status\":\"Converted\",\"conversion\":{\"width\":%u,\"height\":%u,"
                 "\"mipCount\":%u,\"pixelFormat\":\"%s\",\"registered\":%s}}\n",
        info->width, info->height, info->mip_count, pixel_format(info, format_buffer),
        job->options.metadata != NULL ? "true" : "false");
}

typedef struct batch_run {
    native_batch_job *jobs;
    uint32_t converted;
    uint32_t failed;
    uint32_t cancelled;
} batch_run;

typedef struct batch_reporter {
    const native_batch_job *job;
    edds_pool *pool;
    double reported;
} batch_reporter;

/**
 * One row moving while its image converts. Steps below a twentieth are dropped, so a batch of two
 * hundred images cannot flood the one stdout every worker shares.
 */
static void batch_progress(void *context, double progress) {
    batch_reporter *reporter = (batch_reporter *)context;
    if (progress < reporter->reported + 0.05 || progress >= 1.0) {
        return;
    }
    reporter->reported = progress;
    edds_pool_lock_output(reporter->pool);
    write_batch_progress(reporter->job->id, progress);
    (void)fflush(stdout);
    edds_pool_unlock_output(reporter->pool);
}

static edds_status convert_batch_attempt(void *context, edds_error *error) {
    batch_reporter *reporter = context;
    return convert_atomically(&reporter->job->options, &reporter->job->profile,
        batch_progress, reporter, error);
}

/** One image, on whichever worker claimed it. Everything shared is touched under a pool lock. */
static void batch_job_task(void *context, uint32_t at, edds_pool *pool) {
    batch_run *run = (batch_run *)context;
    native_batch_job *job = &run->jobs[at];
    batch_reporter reporter = { NULL, NULL, 0.0 };
    file_revision source_revision;
    edds_error error;
    edds_info info;
    edds_status status;
    uint64_t charge;
    reporter.job = job;
    reporter.pool = pool;
    /* A failure path that names no error still prints one: an empty one, never stack bytes. */
    memset(&error, 0, sizeof error);

    if (was_cancelled(NULL)) {
        memset(&error, 0, sizeof error);
        (void)snprintf(error.code, sizeof error.code, "cancelled");
        (void)snprintf(error.message, sizeof error.message,
            "The batch was cancelled before this job started.");
        edds_pool_lock_output(pool);
        write_batch_failed(job->id, "Cancelled", &error, 0);
        ++run->cancelled;
        (void)fflush(stdout);
        edds_pool_unlock_output(pool);
        return;
    }
    if (job->collision) {
        memset(&error, 0, sizeof error);
        (void)snprintf(error.code, sizeof error.code, "output-collision");
        (void)snprintf(error.message, sizeof error.message,
            "Multiple batch jobs resolve to the same output; no winner was selected.");
        edds_pool_lock_output(pool);
        write_batch_diagnostic(job->id, EDDS_INVALID_INPUT, &error);
        write_batch_failed(job->id, "Failed", &error, 0);
        ++run->failed;
        (void)fflush(stdout);
        edds_pool_unlock_output(pool);
        return;
    }

    edds_pool_lock_output(pool);
    write_batch_progress(job->id, 0.0);
    (void)fflush(stdout);
    edds_pool_unlock_output(pool);

    /* File size is only the initial quota; allocations enforce it even for compressed sources. */
    charge = edds_pool_charge_of(
        revision_of(job->options.input, &source_revision) && source_revision.exists
            ? source_revision.size
            : 0u);
    status = edds_pool_execute(pool, charge, convert_batch_attempt, &reporter,
        was_cancelled, NULL, &error);
    inject_batch_cancel_after_first_commit(at, status);
    if (status == EDDS_OK) {
        status = inspect_converted(job, &info, &error);
    }

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

static int batch_command(void) {
    batch_run run = { NULL, 0, 0, 0 };
    uint32_t count = 0;
    edds_error error;
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

int edds_command(int argc, cli_char **argv) {
    parsed_arguments options;
    FILE *input;
    edds_info info;
    edds_error error;
    edds_metadata metadata;
    edds_metadata *inspected_metadata = NULL;
    char unsupported_metadata_reason[sizeof error.message] = { 0 };
    edds_status status;
    int preview_command;
    int convert;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    cli_catch_interrupts();
    if ((argc == 5 || argc == 7) && equals(argv[1], "batch") &&
        equals(argv[2], "--machine") && equals(argv[3], "--protocol") &&
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
    convert = equals(argv[1], "convert");
    if (!parse_options(argc, argv, 2, &options) || !options.machine ||
        !options.protocol_seen || options.protocol != EDDS_PROTOCOL_VERSION || options.input == NULL ||
        (preview_command && (!options.mip_seen || has_profile_options(&options))) ||
        (!preview_command && options.mip_seen) ||
        (convert && (options.output == NULL || ((options.metadata == NULL) != !has_metadata_identity(&options)) || (options.metadata != NULL && (options.resource_name == NULL || options.source_file == NULL || options.guid == NULL)))) ||
        (has_expected_revisions(&options) &&
            (options.expected_source_revision == NULL || options.expected_output_revision == NULL ||
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
            if (status == EDDS_UNSUPPORTED_FORMAT && options.identity_only) {
                (void)snprintf(unsupported_metadata_reason, sizeof unsupported_metadata_reason,
                    "%s", error.message);
                status = EDDS_OK;
            }
            if (status != EDDS_OK) {
                (void)fclose(input);
                return report_failure(status, &error);
            }
            inspected_metadata = &metadata;
        }
        write_inspection(&info, inspected_metadata,
            unsupported_metadata_reason[0] == '\0' ? NULL : unsupported_metadata_reason);
        (void)fclose(input);
        return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
    }
    status = edds_preview(input, &info, options.mip, was_cancelled, NULL, &rgba, &rgba_size, &error);
    (void)fclose(input);
    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }
    write_preview(&info, options.mip, rgba, rgba_size);
    edds_free(rgba);
    return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
}
