#include <edds/edds.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <wchar.h>
typedef wchar_t cli_char;
#define CLI_ENTRY wmain
#else
typedef char cli_char;
#define CLI_ENTRY main
#endif

enum { EDDS_PROTOCOL_VERSION = 1 };
#define EDDS_TOOL_VERSION "0.1.0"

static volatile sig_atomic_t interrupted = 0;

static void on_interrupt(int signal_value) {
    (void)signal_value;
    interrupted = 1;
}

static int was_cancelled(void *context) {
    (void)context;
    return interrupted != 0;
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

static void usage(void) {
    fputs(
        "usage:\n"
        "  edds-convert protocol --machine\n"
        "  edds-convert inspect --machine --protocol 1 --input PATH\n"
        "  edds-convert preview --machine --protocol 1 --mip N --input PATH\n",
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
    switch (info->pixel_format) {
        case EDDS_PIXEL_BGRA8: return "BGRA8";
        case EDDS_PIXEL_BGRX8: return "BGRX8";
        case EDDS_PIXEL_DXT1: return "DXT1";
        case EDDS_PIXEL_DXT5: return "DXT5";
        case EDDS_PIXEL_DXGI:
            (void)snprintf(buffer, 32, "DXGI_%u", info->dxgi_format);
            return buffer;
        case EDDS_PIXEL_UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

static const char *preview_refusal(const edds_info *info) {
    if (info->pixel_format != EDDS_PIXEL_BGRA8 && info->pixel_format != EDDS_PIXEL_BGRX8) {
        return "Pixel preview is unavailable because this DDS pixel format is not supported.";
    }
    return "Pixel preview is unavailable because only one two-dimensional texture surface is supported.";
}

static void write_inspection(const edds_info *info) {
    char format_buffer[32];
    (void)printf(
        "{\"protocolVersion\":1,\"kind\":\"inspect\",\"width\":%u,\"height\":%u,"
        "\"mipCount\":%u,\"pixelFormat\":\"%s\",\"previewSupported\":%s",
        info->width, info->height, info->mip_count, pixel_format(info, format_buffer),
        info->preview_supported ? "true" : "false"
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
    fputs("]}\n", stdout);
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
        } else {
            return 0;
        }
    }
    return 1;
}

int CLI_ENTRY(int argc, cli_char **argv) {
    parsed_arguments options;
    FILE *input;
    edds_info info;
    edds_error error;
    edds_status status;
    int preview_command;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    (void)signal(SIGINT, on_interrupt);
    if (argc == 3 && equals(argv[1], "protocol") && equals(argv[2], "--machine")) {
        puts("{\"protocolVersion\":1,\"kind\":\"protocol\",\"toolVersion\":\""
            EDDS_TOOL_VERSION "\",\"commands\":[\"inspect\",\"preview\"]}");
        return 0;
    }
    if (argc < 2 || (!equals(argv[1], "inspect") && !equals(argv[1], "preview"))) {
        return invalid_invocation("invalid-command", "Expected protocol, inspect, or preview.");
    }
    preview_command = equals(argv[1], "preview");
    if (!parse_options(argc, argv, 2, &options) || !options.machine ||
        !options.protocol_seen || options.protocol != EDDS_PROTOCOL_VERSION || options.input == NULL ||
        (preview_command && !options.mip_seen) || (!preview_command && options.mip_seen)) {
        return invalid_invocation("invalid-options", "The command options are incomplete, duplicated, or unsupported.");
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
        write_inspection(&info);
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
