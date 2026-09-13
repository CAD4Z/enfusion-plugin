#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <edds/edds.h>
#include <edds/batch.h>
#include <edds/pool.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#include <wchar.h>
typedef wchar_t cli_char;
#define CLI_ENTRY wmain
#define cli_remove _wremove
#define cli_rename _wrename
#define cli_strlen wcslen
#define cli_strcmp wcscmp
#define cli_getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
typedef char cli_char;
#define CLI_ENTRY main
#define cli_remove remove
#define cli_rename rename
#define cli_strlen strlen
#define cli_strcmp strcmp
#define cli_getpid getpid
#endif

enum { EDDS_PROTOCOL_VERSION = 1 };
#define EDDS_PROTOCOL_TEXT "1"
#define EDDS_TOOL_VERSION "0.1.0"

static volatile sig_atomic_t interrupted = 0;
static const cli_char *batch_cancel_file = NULL;
static int path_exists(const cli_char *path);

static void on_interrupt(int signal_value) {
    (void)signal_value;
    interrupted = 1;
}

static int was_cancelled(void *context) {
    (void)context;
    return interrupted != 0 || (batch_cancel_file != NULL && path_exists(batch_cancel_file));
}

static int equals(const cli_char *left, const char *right) {
#ifdef _WIN32
    while (*left != L'\0' && *right != '\0' && *left == (wchar_t)(unsigned char)*right) {
        ++left;
        ++right;
    }
    return *left == L'\0' && *right == '\0';
#else
    return strcmp(left, right) == 0;
#endif
}

static FILE *open_input(const cli_char *path) {
#ifdef _WIN32
    FILE *file = NULL;
    return _wfopen_s(&file, path, L"rb") == 0 ? file : NULL;
#else
    return fopen(path, "rb");
#endif
}

static FILE *open_output(const cli_char *path) {
#ifdef _WIN32
    FILE *file = NULL;
    return _wfopen_s(&file, path, L"w+b") == 0 ? file : NULL;
#else
    return fopen(path, "w+b");
#endif
}

static int injected(const char *stage) {
    const char *requested = getenv("EDDS_CONVERT_FAIL");
    return requested != NULL && strcmp(requested, stage) == 0;
}

static edds_status injected_failure(edds_error *error, const char *stage) {
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "injected-%s", stage);
    (void)snprintf(error->message, sizeof error->message,
        "A test fault was injected at transaction stage %s.", stage);
    return EDDS_INTERNAL_FAILURE;
}

static int sync_output(FILE *file, const char *stage) {
    if (injected(stage)) {
        return 0;
    }
    if (fflush(file) != 0) {
        return 0;
    }
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static int unsigned_argument(const cli_char *text, uint32_t *value) {
    uint64_t parsed = 0;
    if (*text == 0) {
        return 0;
    }
    while (*text != 0) {
        unsigned digit;
        if (*text < (cli_char)'0' || *text > (cli_char)'9') {
            return 0;
        }
        digit = (unsigned)(*text - (cli_char)'0');
        parsed = parsed * 10u + digit;
        if (parsed > UINT32_MAX) {
            return 0;
        }
        ++text;
    }
    *value = (uint32_t)parsed;
    return 1;
}

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
        if (whole > 1u) return 0;
        seen = 1;
    }
    if (!seen) return 0;
    if (*text == (cli_char)'.') {
        ++text;
        if (*text < (cli_char)'0' || *text > (cli_char)'9') return 0;
        while (*text >= (cli_char)'0' && *text <= (cli_char)'9') {
            if (digits >= 3u) return 0;
            fraction = fraction * 10u + (uint32_t)(*text++ - (cli_char)'0');
            ++digits;
        }
    }
    if (*text != 0) return 0;
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
        "  edds-convert protocol --machine\n"
        "  edds-convert inspect --machine --protocol 1 --input PATH\n"
        "  edds-convert preview --machine --protocol 1 --mip N --input PATH\n"
        "  edds-convert batch --machine --protocol 1 < jobs.ndjson\n"
        "  edds-convert convert --machine --protocol 1 --input PATH --output PATH [PROFILE FLAGS]\n",
        stderr
    );
}

static void json_string(const char *text) {
    const unsigned char *at = (const unsigned char *)text;
    putchar('"');
    while (*at != 0) {
        switch (*at) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*at < 0x20u) {
                    (void)printf("\\u%04x", (unsigned)*at);
                } else {
                    putchar(*at);
                }
                break;
        }
        ++at;
    }
    putchar('"');
}

static int report_failure(edds_status status, const edds_error *error) {
    const char *category = edds_status_category(status);
    const char *code = error != NULL && error->code[0] != '\0' ? error->code : "unspecified";
    const char *message = error != NULL && error->message[0] != '\0'
        ? error->message : "The EDDS operation failed.";
    (void)fprintf(stderr, "edds-convert: %s: %s\n", category, message);
    fputs("{\"protocolVersion\":1,\"kind\":\"error\",\"error\":{\"category\":", stdout);
    json_string(category);
    fputs(",\"code\":", stdout);
    json_string(code);
    fputs(",\"message\":", stdout);
    json_string(message);
    fputs("}}\n", stdout);
    return (int)status;
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
    const char *unsupported_metadata_reason
) {
    char format_buffer[32];
    char quality_buffer[8];
    /* The channels a decode of the file carries, which is the runtime fact, not the source's. */
    const char *channels = edds_pixel_format_channels(info->pixel_format);
    (void)printf(
        "{\"protocolVersion\":1,\"kind\":\"inspect\",\"width\":%u,\"height\":%u,"
        "\"mipCount\":%u,\"pixelFormat\":\"%s\",\"channels\":\"%s\",\"previewSupported\":%s",
        info->width, info->height, info->mip_count, pixel_format(info, format_buffer),
        channels, info->preview_supported ? "true" : "false"
    );
    if (!info->preview_supported) {
        fputs(",\"previewUnsupportedReason\":", stdout);
        json_string(preview_refusal(info));
    }
    (void)printf(
        ",\"dds\":{\"flags\":%u,\"pitchOrLinearSize\":%u,\"depth\":%u,"
        "\"pixelFormatFlags\":%u,\"fourCC\":",
        info->flags, info->pitch_or_linear_size, info->depth, info->pixel_format_flags
    );
    json_string(info->four_cc);
    (void)printf(
        ",\"rgbBitCount\":%u,\"rMask\":%u,\"gMask\":%u,\"bMask\":%u,\"aMask\":%u,"
        "\"caps\":%u,\"caps2\":%u,\"dxgiFormat\":%u,"
        "\"resourceDimension\":%u,\"arraySize\":%u,\"miscFlag\":%u},\"mips\":[",
        info->rgb_bit_count, info->r_mask, info->g_mask, info->b_mask, info->a_mask,
        info->caps, info->caps2,
        info->dxgi_format, info->resource_dimension, info->array_size, info->misc_flag
    );
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
            (unsigned long long)mip->data_offset
        );
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
        (void)printf(",\"CompressTreshold\":%u,\"Conversion\":", metadata->profile.compress_threshold);
        {
            const edds_conversion_capability *conversion =
                edds_conversion_capability_of(metadata->profile.conversion);
            json_string(conversion == NULL ? "None" : conversion->workbench_name);
        }
        (void)printf(
            ",\"ConversionQuality\":%s,"
            "\"Swizzling\":\"None\",\"GenerateMips\":%s,"
            "\"MipMapFunction\":\"Filter\",\"MipMapFilter\":\"Box\",\"TiledTexture\":true}}",
            quality_json(metadata->profile.conversion_quality, quality_buffer),
            metadata->profile.generate_mips ? "true" : "false");
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
        mip, selected->width, selected->height, (unsigned long long)size
    );
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
    const cli_char *conversion;
    int quality_seen;
    uint32_t conversion_quality;
    const cli_char *swizzling;
    const cli_char *generate_mips;
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
    int identity_only;
} parsed_arguments;

static int parse_options(int argc, cli_char **argv, int first, parsed_arguments *options) {
    memset(options, 0, sizeof *options);
    for (int at = first; at < argc; ++at) {
        if (equals(argv[at], "--machine") && !options->machine) {
            options->machine = 1;
        } else if (equals(argv[at], "--protocol") && !options->protocol_seen && at + 1 < argc) {
            options->protocol_seen = unsigned_argument(argv[++at], &options->protocol);
            if (!options->protocol_seen) return 0;
        } else if (equals(argv[at], "--mip") && !options->mip_seen && at + 1 < argc) {
            options->mip_seen = unsigned_argument(argv[++at], &options->mip);
            if (!options->mip_seen) return 0;
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
            if (!options->threshold_seen) return 0;
        } else if (equals(argv[at], "--conversion") && options->conversion == NULL && at + 1 < argc) {
            options->conversion = argv[++at];
        } else if (equals(argv[at], "--conversion-quality") && !options->quality_seen && at + 1 < argc) {
            options->quality_seen = quality_argument(argv[++at], &options->conversion_quality);
            if (!options->quality_seen) return 0;
        } else if (equals(argv[at], "--swizzling") && options->swizzling == NULL && at + 1 < argc) {
            options->swizzling = argv[++at];
        } else if (equals(argv[at], "--generate-mips") && options->generate_mips == NULL && at + 1 < argc) {
            options->generate_mips = argv[++at];
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
        } else {
            return 0;
        }
    }
    return 1;
}

static int has_profile_options(const parsed_arguments *options) {
    return options->output != NULL || options->target_format != NULL ||
        options->format_compress != NULL || options->threshold_seen ||
        options->conversion != NULL || options->quality_seen || options->swizzling != NULL ||
        options->generate_mips != NULL || options->mipmap_function != NULL ||
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
    edds_error *error
) {
    edds_default_profile(profile);
    if (options->target_format != NULL && !equals(options->target_format, "enfusion-dds")) {
        goto unsupported;
    }
    if (options->format_compress != NULL) {
        if (equals(options->format_compress, "copy")) profile->format_compress = EDDS_COMPRESS_COPY;
        else if (equals(options->format_compress, "fastest")) profile->format_compress = EDDS_COMPRESS_FASTEST;
        else if (equals(options->format_compress, "medium")) profile->format_compress = EDDS_COMPRESS_MEDIUM;
        else if (equals(options->format_compress, "best")) profile->format_compress = EDDS_COMPRESS_BEST;
        else goto unsupported;
    }
    if (options->threshold_seen) {
        if (options->compress_threshold > 100u) goto unsupported;
        profile->compress_threshold = options->compress_threshold;
    }
    if (options->conversion != NULL) {
        size_t count = 0;
        const edds_conversion_capability *capabilities = edds_conversions(&count);
        const edds_conversion_capability *chosen = NULL;
        for (size_t at = 0; at < count; ++at) {
            if (equals(options->conversion, capabilities[at].wire_name)) chosen = &capabilities[at];
        }
        if (chosen == NULL) goto unsupported;
        profile->conversion = chosen->conversion;
    }
    if (options->quality_seen) {
        profile->conversion_quality = options->conversion_quality;
    }
    if (options->swizzling != NULL && !equals(options->swizzling, "none")) goto unsupported;
    if (options->generate_mips != NULL) {
        if (equals(options->generate_mips, "true")) profile->generate_mips = 1;
        else if (equals(options->generate_mips, "false")) profile->generate_mips = 0;
        else goto unsupported;
    }
    if (options->mipmap_function != NULL && !equals(options->mipmap_function, "filter")) goto unsupported;
    if (options->mipmap_filter != NULL && !equals(options->mipmap_filter, "box")) goto unsupported;
    if (options->tiled_texture != NULL && !equals(options->tiled_texture, "true")) goto unsupported;
    /* The conversion and its quality are one combination, so they are judged as one. */
    return edds_profile_check(profile, error);

unsupported:
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "unsupported-setting");
    (void)snprintf(error->message, sizeof error->message,
        "A requested Workbench profile value is recognized but not supported by this conversion slice.");
    return EDDS_UNSUPPORTED_FORMAT;
}

static int ascii_lower(cli_char value) {
    return value >= (cli_char)'A' && value <= (cli_char)'Z'
        ? value + ((cli_char)'a' - (cli_char)'A') : value;
}

static int ends_with(const cli_char *path, const char *suffix) {
    const size_t path_size = cli_strlen(path);
    const size_t suffix_size = strlen(suffix);
    if (path_size < suffix_size) return 0;
    for (size_t at = 0; at < suffix_size; ++at) {
        if (ascii_lower(path[path_size - suffix_size + at]) != (unsigned char)suffix[at]) return 0;
    }
    return 1;
}

/**
 * A destination reduced to what a comparison should look at: case, either separator, a doubled or
 * trailing one, a `.` segment, and a `..` the path walks back through. A root is never walked
 * above, so a drive and a UNC share stay whole. Returns a malloc'd normal form, or NULL.
 */
static int is_separator(cli_char value) {
    return value == (cli_char)'/' || value == (cli_char)'\\';
}

static cli_char *normalized_path(const cli_char *path) {
    const size_t length = cli_strlen(path);
    cli_char *result = malloc((length + 2u) * sizeof *result);
    /* Where each segment that may still be walked off begins; a root segment is never in here. */
    size_t *marks = malloc((length / 2u + 2u) * sizeof *marks);
    size_t depth = 0;
    size_t out = 0;
    size_t at = 0;
    size_t prefix;
    size_t roots;
    int share;
    int anchored;
    if (result == NULL || marks == NULL) {
        free(result);
        free(marks);
        return NULL;
    }
    /* A `/C:` the way a Uri.path spells a drive is not a rooted path, it is the drive itself. */
    if (length >= 3u && is_separator(path[0]) && path[2] == (cli_char)':' &&
        ascii_lower(path[1]) >= (cli_char)'a' && ascii_lower(path[1]) <= (cli_char)'z') {
        at = 1u;
    }
    share = length >= at + 2u && is_separator(path[at]) && is_separator(path[at + 1u]);
    if (share) {
        result[out++] = (cli_char)'/';
        result[out++] = (cli_char)'/';
        at += 2u;
    } else if (at < length && is_separator(path[at])) {
        result[out++] = (cli_char)'/';
        ++at;
    }
    prefix = out;
    /* The server and the share name a UNC root; a bare drive letter is a root of its own. */
    roots = share ? 2u : 0u;
    anchored = prefix > 0u;

    while (at < length) {
        const size_t start = at;
        size_t size;
        int parent;
        while (at < length && !is_separator(path[at])) ++at;
        size = at - start;
        if (at < length) ++at;
        if (size == 0u || (size == 1u && path[start] == (cli_char)'.')) continue;
        parent = size == 2u && path[start] == (cli_char)'.' && path[start + 1u] == (cli_char)'.';
        if (parent && depth > 0u) {
            out = marks[--depth];
            continue;
        }
        /* Nothing sits above a root, so the walk a rooted path cannot take is dropped, not kept. */
        if (parent && anchored) continue;
        if (out > prefix) result[out++] = (cli_char)'/';
        /* A root segment and a `..` a relative path kept are both segments nothing walks off. */
        if (roots > 0u) {
            --roots;
        } else if (!parent) {
            marks[depth++] = out > prefix ? out - 1u : out;
        }
        for (size_t copy = 0; copy < size; ++copy) {
            result[out++] = (cli_char)ascii_lower(path[start + copy]);
        }
        if (!anchored && depth == 1u && out == 2u && result[1] == (cli_char)':') {
            /* That first segment was a drive letter after all: it becomes the root behind us. */
            depth = 0;
            anchored = 1;
        }
    }
    result[out] = 0;
    free(marks);
    return result;
}

/** Case and separator only: what a comparison can still say when there is no memory to normalize. */
static int same_path_literally(const cli_char *left, const cli_char *right) {
    while (*left != 0 && *right != 0) {
        const cli_char a = is_separator(*left) ? (cli_char)'/' : (cli_char)ascii_lower(*left);
        const cli_char b = is_separator(*right) ? (cli_char)'/' : (cli_char)ascii_lower(*right);
        if (a != b) return 0;
        ++left;
        ++right;
    }
    return *left == 0 && *right == 0;
}

static int same_path(const cli_char *left, const cli_char *right) {
    cli_char *canonical_left = normalized_path(left);
    cli_char *canonical_right = normalized_path(right);
    /* Out of memory, the coarser answer is the safe one: a collision missed writes two jobs to
       one file, while a collision seen twice only refuses work the caller can ask for again. */
    const int same = canonical_left == NULL || canonical_right == NULL
        ? same_path_literally(left, right)
        : cli_strcmp(canonical_left, canonical_right) == 0;
    free(canonical_left);
    free(canonical_right);
    return same;
}

static cli_char *temporary_path(const cli_char *output, const char *kind, unsigned attempt) {
    const size_t base = cli_strlen(output);
    cli_char *path = malloc((base + 96u) * sizeof *path);
    if (path == NULL) return NULL;
#ifdef _WIN32
    if (swprintf(path, base + 96u, L"%ls.edds-convert-%hs-%d-%u.tmp",
            output, kind, cli_getpid(), attempt) < 0) {
#else
    if (snprintf(path, base + 96u, "%s.edds-convert-%s-%d-%u.tmp",
            output, kind, cli_getpid(), attempt) < 0) {
#endif
        free(path);
        return NULL;
    }
    return path;
}

static int path_exists(const cli_char *path) {
    FILE *file = open_input(path);
    if (file == NULL) return 0;
    (void)fclose(file);
    return 1;
}

static cli_char *append_suffix(const cli_char *path, const char *suffix);

typedef struct file_revision {
    int exists;
    uint64_t size;
    uint64_t modified;
} file_revision;

static int revision_of(const cli_char *path, file_revision *revision) {
    memset(revision, 0, sizeof *revision);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    ULARGE_INTEGER size;
    ULARGE_INTEGER time;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) {
        const DWORD code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return 1;
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    time.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    time.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    revision->exists = 1;
    revision->size = size.QuadPart;
    revision->modified = time.QuadPart / 10000u - 11644473600000u;
#else
    struct stat attributes;
    if (stat(path, &attributes) != 0) return errno == ENOENT;
    if (!S_ISREG(attributes.st_mode)) return 1;
    revision->exists = 1;
    revision->size = (uint64_t)attributes.st_size;
    revision->modified = (uint64_t)attributes.st_mtim.tv_sec * 1000u +
        (uint64_t)attributes.st_mtim.tv_nsec / 1000000u;
#endif
    return 1;
}

static int parse_revision(const cli_char *text, file_revision *revision) {
    uint64_t values[2] = { 0, 0 };
    memset(revision, 0, sizeof *revision);
    if (equals(text, "missing")) return 1;
    for (unsigned part = 0; part < 2u; ++part) {
        int digits = 0;
        while (*text >= (cli_char)'0' && *text <= (cli_char)'9') {
            const unsigned digit = (unsigned)(*text - (cli_char)'0');
            if (values[part] > (UINT64_MAX - digit) / 10u) return 0;
            values[part] = values[part] * 10u + digit;
            ++text;
            digits = 1;
        }
        if (!digits || (part == 0u ? *text++ != (cli_char)':' : *text != 0)) return 0;
    }
    revision->exists = 1;
    revision->size = values[0];
    revision->modified = values[1];
    return 1;
}

static int same_revision(const file_revision *left, const file_revision *right) {
    return left->exists == right->exists && (!left->exists ||
        (left->size == right->size && left->modified == right->modified));
}

static edds_status validate_revisions(
    const parsed_arguments *options,
    edds_error *error
) {
    cli_char *metadata_path;
    file_revision actual;
    file_revision expected;
    const cli_char *paths[3];
    const cli_char *values[3];
    const char *names[3] = { "source", "output", "metadata" };
    if (!has_expected_revisions(options)) return EDDS_OK;
    metadata_path = append_suffix(options->output, ".meta");
    if (metadata_path == NULL) return EDDS_INTERNAL_FAILURE;
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

static cli_char *append_suffix(const cli_char *path, const char *suffix) {
    const size_t path_size = cli_strlen(path);
    const size_t suffix_size = strlen(suffix);
    cli_char *result = malloc((path_size + suffix_size + 1u) * sizeof *result);
    if (result == NULL) return NULL;
    memcpy(result, path, path_size * sizeof *result);
    for (size_t at = 0; at < suffix_size; ++at) {
        result[path_size + at] = (cli_char)(unsigned char)suffix[at];
    }
    result[path_size + suffix_size] = 0;
    return result;
}

static char *utf8_of(const cli_char *value) {
#ifdef _WIN32
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
        NULL, 0, NULL, NULL);
    char *result;
    if (needed <= 0) return NULL;
    result = malloc((size_t)needed);
    if (result == NULL) return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
            result, needed, NULL, NULL) != needed) {
        free(result);
        return NULL;
    }
    return result;
#else
    const size_t size = strlen(value);
    char *result = malloc(size + 1u);
    if (result != NULL) memcpy(result, value, size + 1u);
    return result;
#endif
}

static int metadata_text(char *destination, size_t capacity, const char *source) {
    const size_t size = strlen(source);
    if (size >= capacity) return 0;
    memcpy(destination, source, size + 1u);
    return 1;
}

typedef struct cli_artifact {
    const cli_char *target;
    cli_char *temporary;
    cli_char *backup;
    int had_previous;
    int committed;
} cli_artifact;

static FILE *create_temporary(cli_artifact *artifact, const char *kind) {
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        FILE *file;
        free(artifact->temporary);
        artifact->temporary = temporary_path(artifact->target, kind, attempt);
        if (artifact->temporary == NULL || path_exists(artifact->temporary)) continue;
        file = open_output(artifact->temporary);
        if (file != NULL) return file;
    }
    return NULL;
}

static int backup_artifact(cli_artifact *artifact, const char *kind, const char *stage) {
    artifact->had_previous = path_exists(artifact->target);
    if (!artifact->had_previous) return 1;
    if (injected(stage)) return 0;
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        free(artifact->backup);
        artifact->backup = temporary_path(artifact->target, kind, attempt);
        if (artifact->backup != NULL && !path_exists(artifact->backup) &&
            cli_rename(artifact->target, artifact->backup) == 0) return 1;
    }
    return 0;
}

static int commit_artifact(cli_artifact *artifact, const char *stage) {
    if (injected(stage)) return 0;
    if (cli_rename(artifact->temporary, artifact->target) != 0) return 0;
    artifact->committed = 1;
    return 1;
}

static void rollback_artifact(cli_artifact *artifact) {
    if (artifact->committed) {
        (void)cli_remove(artifact->target);
        artifact->committed = 0;
    }
    if (artifact->had_previous && artifact->backup != NULL) {
        (void)cli_rename(artifact->backup, artifact->target);
    }
}

static void cleanup_artifact(cli_artifact *artifact, int success) {
    if (artifact->temporary != NULL) (void)cli_remove(artifact->temporary);
    if (success && artifact->backup != NULL) (void)cli_remove(artifact->backup);
    free(artifact->temporary);
    free(artifact->backup);
    artifact->temporary = NULL;
    artifact->backup = NULL;
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
    edds_error *error
) {
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
    edds_error *error
) {
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
    edds_error *error
) {
    FILE *input;
    edds_metadata previous;
    edds_status status;
    if (!path_exists(options->metadata)) return EDDS_OK;
    input = open_input(options->metadata);
    if (input == NULL) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "metadata-open-failed");
        (void)snprintf(error->message, sizeof error->message, "Existing metadata could not be opened.");
        return EDDS_INVALID_INPUT;
    }
    status = edds_metadata_parse(input, &previous, error);
    (void)fclose(input);
    if (status != EDDS_OK) return status;
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
    edds_error *error
) {
    FILE *source = NULL;
    FILE *temporary_output = NULL;
    FILE *temporary_metadata = NULL;
    cli_artifact output_artifact = { options->output, NULL, NULL, 0, 0 };
    cli_artifact metadata_artifact = { options->metadata, NULL, NULL, 0, 0 };
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
    if (status != EDDS_OK) return status;
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
    if (status != EDDS_OK) return status;
    if (registered) {
        status = metadata_value_of(options, format, profile, &metadata, error);
        if (status != EDDS_OK) return status;
        status = validate_previous_metadata(options, &metadata, error);
        if (status != EDDS_OK) return status;
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
    if (fclose(source) != 0 && status == EDDS_OK) status = EDDS_INVALID_INPUT;
    if (fclose(temporary_output) != 0 && status == EDDS_OK) status = EDDS_INTERNAL_FAILURE;
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
                ? injected_failure(error, "metadata-flush") : EDDS_INTERNAL_FAILURE;
        }
        if (fclose(temporary_metadata) != 0 && status == EDDS_OK) status = EDDS_INTERNAL_FAILURE;
        if (status != EDDS_OK) {
            cleanup_artifact(&output_artifact, 0);
            cleanup_artifact(&metadata_artifact, 0);
            return status;
        }
    }
    status = validate_revisions(options, error);
    if (status != EDDS_OK) {
        cleanup_artifact(&output_artifact, 0);
        cleanup_artifact(&metadata_artifact, 0);
        return status;
    }
    output_backed = backup_artifact(&output_artifact, "old", "output-backup-rename");
    if (output_backed && registered) {
        metadata_backed = backup_artifact(&metadata_artifact, "old", "metadata-backup-rename");
    }
    if (!output_backed || (registered && !metadata_backed)) {
        rollback_artifact(&metadata_artifact);
        rollback_artifact(&output_artifact);
        cleanup_artifact(&output_artifact, 0);
        cleanup_artifact(&metadata_artifact, 0);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "artifact-backup-failed");
        (void)snprintf(error->message, sizeof error->message, "The previous EDDS/metadata pair could not be moved aside atomically.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (!commit_artifact(&output_artifact, "output-commit-rename") ||
        (registered && !commit_artifact(&metadata_artifact, "metadata-commit-rename"))) {
        rollback_artifact(&metadata_artifact);
        rollback_artifact(&output_artifact);
        cleanup_artifact(&output_artifact, 0);
        cleanup_artifact(&metadata_artifact, 0);
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "artifact-commit-failed");
        (void)snprintf(error->message, sizeof error->message, "The converted EDDS/metadata pair could not replace its destination.");
        return EDDS_INTERNAL_FAILURE;
    }
    cleanup_artifact(&output_artifact, 1);
    cleanup_artifact(&metadata_artifact, 1);
    return EDDS_OK;
}

static int convert_command(const parsed_arguments *options) {
    edds_profile profile;
    edds_error error;
    edds_info info;
    edds_status status = profile_of(options, &profile, &error);
    FILE *output;
    if (status != EDDS_OK) return report_failure(status, &error);
    status = convert_atomically(options, &profile, NULL, NULL, &error);
    if (status != EDDS_OK) return report_failure(status, &error);
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
    if (status != EDDS_OK) return report_failure(status, &error);
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

static cli_char *cli_of_utf8(const char *value) {
#ifdef _WIN32
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    cli_char *result;
    if (needed <= 0) return NULL;
    result = malloc((size_t)needed * sizeof *result);
    if (result == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, needed) != needed) {
        free(result);
        return NULL;
    }
    return result;
#else
    const size_t size = strlen(value);
    cli_char *result = malloc(size + 1u);
    if (result != NULL) memcpy(result, value, size + 1u);
    return result;
#endif
}

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
        (source->has_metadata && (job->metadata == NULL || job->resource_name == NULL ||
            job->source_file == NULL || job->guid == NULL)) ||
        (source->has_expected && (job->expected_source == NULL || job->expected_output == NULL ||
            job->expected_metadata == NULL))) {
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
            if (batch_stdin.size == 0u) batch_stdin.ended = 1;
            continue;
        } else {
            framed = edds_batch_reader_finish(&batch_stdin.reader, &length);
            if (framed == EDDS_BATCH_LINE_PENDING) return 0;
        }
        if (framed == EDDS_BATCH_LINE_OVERFLOW || (framed == EDDS_BATCH_LINE_READY &&
                length + 1u > capacity)) {
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
    edds_error *error
) {
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
                memset(error, 0, sizeof *error);
                (void)snprintf(error->code, sizeof error->code, "invalid-batch-job");
                (void)snprintf(error->message, sizeof error->message,
                    "A batch job path or identity is not valid UTF-8 for this platform.");
                status = EDDS_INVALID_INVOCATION;
            }
            for (uint32_t free_at = 0; free_at <= at; ++free_at) free_batch_job(&loaded[free_at]);
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
                for (uint32_t free_at = 0; free_at <= at; ++free_at) free_batch_job(&loaded[free_at]);
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
        for (uint32_t at = 0; at < *count; ++at) free_batch_job(&loaded[at]);
        free(loaded);
        free(line);
        free(record);
        return read < 0 ? EDDS_INVALID_INVOCATION : status;
    }
    /* Normalized once each, so comparing every destination against every other stays cheap. */
    {
        cli_char **destinations = calloc(*count, sizeof *destinations);
        for (uint32_t at = 0; destinations != NULL && at < *count; ++at) {
            destinations[at] = normalized_path(loaded[at].output);
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
        for (uint32_t at = 0; destinations != NULL && at < *count; ++at) free(destinations[at]);
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
    (void)fprintf(stderr, "edds-convert: %s: %s\n", edds_status_category(status), error->message);
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
    int retryable
) {
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
    FILE *marker;
    if (at != 0u || status != EDDS_OK || batch_cancel_file == NULL ||
        !injected("batch-cancel-after-first-commit")) return;
    marker = open_output(batch_cancel_file);
    if (marker != NULL) (void)fclose(marker);
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
    const char *id;
    edds_pool *pool;
    double reported;
} batch_reporter;

/**
 * One row moving while its image converts. Steps below a twentieth are dropped, so a batch of two
 * hundred images cannot flood the one stdout every worker shares.
 */
static void batch_progress(void *context, double progress) {
    batch_reporter *reporter = (batch_reporter *)context;
    if (progress < reporter->reported + 0.05 || progress >= 1.0) return;
    reporter->reported = progress;
    edds_pool_lock_output(reporter->pool);
    write_batch_progress(reporter->id, progress);
    (void)fflush(stdout);
    edds_pool_unlock_output(reporter->pool);
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
    reporter.id = job->id;
    reporter.pool = pool;

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

    /* The image only starts decoding once its share of the one memory budget is free. */
    charge = edds_pool_charge_of(
        revision_of(job->options.input, &source_revision) && source_revision.exists
            ? source_revision.size
            : 0u);
    edds_pool_reserve(pool, charge);
    status = convert_atomically(&job->options, &job->profile, batch_progress, &reporter, &error);
    inject_batch_cancel_after_first_commit(at, status);
    if (status == EDDS_OK) status = inspect_converted(job, &info, &error);
    edds_pool_release(pool, charge);

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
    if (status != EDDS_OK) return report_failure(status, &error);
    (void)printf("{\"protocolVersion\":1,\"kind\":\"batch-started\",\"jobCount\":%u}\n", count);
    (void)fflush(stdout);
    status = edds_pool_run(count, EDDS_POOL_MEMORY_BUDGET, batch_job_task, &run, &error);
    if (status != EDDS_OK) {
        for (uint32_t at = 0; at < count; ++at) free_batch_job(&run.jobs[at]);
        free(run.jobs);
        return report_failure(status, &error);
    }
    (void)printf("{\"protocolVersion\":1,\"kind\":\"complete\",\"converted\":%u,"
        "\"failed\":%u,\"cancelled\":%u}\n", run.converted, run.failed, run.cancelled);
    for (uint32_t at = 0; at < count; ++at) free_batch_job(&run.jobs[at]);
    free(run.jobs);
    if (ferror(stdout)) return EDDS_INTERNAL_FAILURE;
    return was_cancelled(NULL) ? EDDS_CANCELLED : 0;
}

int CLI_ENTRY(int argc, cli_char **argv) {
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

    (void)signal(SIGINT, on_interrupt);
    if (argc == 3 && equals(argv[1], "protocol") && equals(argv[2], "--machine")) {
        puts("{\"protocolVersion\":1,\"kind\":\"protocol\",\"toolVersion\":\""
            EDDS_TOOL_VERSION "\",\"commands\":[\"inspect\",\"preview\",\"convert\",\"batch\"]}");
        return 0;
    }
    if ((argc == 5 || argc == 7) && equals(argv[1], "batch") &&
        equals(argv[2], "--machine") && equals(argv[3], "--protocol") &&
        equals(argv[4], EDDS_PROTOCOL_TEXT) &&
        (argc == 5 || equals(argv[5], "--cancel-file"))) {
        int result;
        batch_cancel_file = argc == 7 ? argv[6] : NULL;
        result = batch_command();
        batch_cancel_file = NULL;
        return result;
    }
    if (argc < 2 || (!equals(argv[1], "inspect") && !equals(argv[1], "preview") &&
        !equals(argv[1], "convert"))) {
        return invalid_invocation("invalid-command", "Expected protocol, inspect, preview, convert, or batch.");
    }
    preview_command = equals(argv[1], "preview");
    convert = equals(argv[1], "convert");
    if (!parse_options(argc, argv, 2, &options) || !options.machine ||
        !options.protocol_seen || options.protocol != EDDS_PROTOCOL_VERSION || options.input == NULL ||
        (preview_command && (!options.mip_seen || has_profile_options(&options))) ||
        (!preview_command && options.mip_seen) ||
        (convert && (options.output == NULL ||
            ((options.metadata == NULL) != !has_metadata_identity(&options)) ||
            (options.metadata != NULL &&
                (options.resource_name == NULL || options.source_file == NULL || options.guid == NULL)))) ||
        (has_expected_revisions(&options) &&
            (options.expected_source_revision == NULL || options.expected_output_revision == NULL ||
                options.expected_metadata_revision == NULL)) ||
        (!convert && has_expected_revisions(&options)) ||
        (!convert && (has_profile_options(&options) || has_metadata_identity(&options))) ||
        (preview_command && options.metadata != NULL) ||
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
        return 0;
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
