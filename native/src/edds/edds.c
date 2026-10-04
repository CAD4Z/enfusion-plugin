#include "memory.h"
#include <edds/edds.h>

#include "gpu.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum {
    DDS_HEADER_BYTES = 128,
    DDS_DX10_HEADER_BYTES = 20,
    DDS_RESOURCE_DIMENSION_TEXTURE2D = 3,
    DDS_RESOURCE_MISC_TEXTURECUBE = 4,
    DDSCAPS2_CUBEMAP = 0x00000200,
    DDSCAPS2_VOLUME = 0x00200000,
    DDPF_ALPHAPIXELS = 0x00000001,
    DDPF_FOURCC = 0x00000004,
    DDPF_RGB = 0x00000040,
    LZ4_DECODED_BLOCK_BYTES = 65536
};

typedef struct stored_mip {
    edds_container container;
    uint32_t stored_bytes;
    uint32_t decoded_bytes;
    uint32_t block_count;
    uint64_t data_offset;
} stored_mip;

static uint32_t u32le(const uint8_t *at) {
    return (uint32_t)at[0] |
        ((uint32_t)at[1] << 8) |
        ((uint32_t)at[2] << 16) |
        ((uint32_t)at[3] << 24);
}

static void fail(edds_error *error, const char *code, const char *format, ...) {
    va_list arguments;
    if (error == NULL) {
        return;
    }
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof error->message, format, arguments);
    va_end(arguments);
}

static int cancelled(edds_cancelled_fn callback, void *context, edds_error *error) {
    if (callback != NULL && callback(context)) {
        fail(error, "cancelled", "The EDDS operation was cancelled.");
        return 1;
    }
    return 0;
}

static int read_exact(FILE *input, void *destination, size_t size) {
    return size == 0 || fread(destination, 1, size, input) == size;
}

static int seek_to(FILE *input, uint64_t offset) {
    if (offset > EDDS_MAX_FILE_BYTES) {
        return 0;
    }
    return fseek(input, (long)offset, SEEK_SET) == 0;
}

static int file_size(FILE *input, uint64_t *size) {
    long end;
    if (fseek(input, 0, SEEK_END) != 0) {
        return 0;
    }
    end = ftell(input);
    if (end < 0 || fseek(input, 0, SEEK_SET) != 0) {
        return 0;
    }
    *size = (uint64_t)(unsigned long)end;
    return 1;
}

static uint32_t mip_dimension(uint32_t base, uint32_t level) {
    while (level > 0 && base > 1) {
        base >>= 1;
        --level;
    }
    return base == 0 ? 1 : base;
}

static int expected_rgba_bytes(uint32_t width, uint32_t height, uint32_t *bytes) {
    const uint64_t value = (uint64_t)width * height * 4u;
    if (value == 0 || value > EDDS_MAX_PREVIEW_BYTES) {
        return 0;
    }
    *bytes = (uint32_t)value;
    return 1;
}

static edds_pixel_format classify(const edds_info *info) {
    if ((info->pixel_format_flags & DDPF_FOURCC) != 0) {
        if (strcmp(info->four_cc, "DXT1") == 0) {
            return EDDS_PIXEL_DXT1;
        }
        if (strcmp(info->four_cc, "DXT5") == 0) {
            return EDDS_PIXEL_DXT5;
        }
        if (strcmp(info->four_cc, "DX10") == 0) {
            switch (info->dxgi_format) {
                case 87:
                case 91: return EDDS_PIXEL_BGRA8;
                case 88:
                case 93: return EDDS_PIXEL_BGRX8;
                case 61: return EDDS_PIXEL_R8;
                case 49: return EDDS_PIXEL_RG8;
                case 80: return EDDS_PIXEL_BC4;
                case 83: return EDDS_PIXEL_BC5;
                case 98: return EDDS_PIXEL_BC7;
                default: return EDDS_PIXEL_DXGI;
            }
        }
        return EDDS_PIXEL_UNKNOWN;
    }
    if ((info->pixel_format_flags & DDPF_RGB) != 0 && info->rgb_bit_count == 32 &&
        info->r_mask == 0x00ff0000u && info->g_mask == 0x0000ff00u &&
        info->b_mask == 0x000000ffu) {
        if ((info->pixel_format_flags & DDPF_ALPHAPIXELS) != 0 &&
            info->a_mask == 0xff000000u) {
            return EDDS_PIXEL_BGRA8;
        }
        if ((info->pixel_format_flags & DDPF_ALPHAPIXELS) == 0 && info->a_mask == 0) {
            return EDDS_PIXEL_BGRX8;
        }
    }
    return EDDS_PIXEL_UNKNOWN;
}

static int topology_is_previewable(const edds_info *info) {
    if ((info->caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME)) != 0 || info->depth > 1) {
        return 0;
    }
    if ((info->pixel_format_flags & DDPF_FOURCC) != 0 &&
        strcmp(info->four_cc, "DX10") == 0) {
        return info->resource_dimension == DDS_RESOURCE_DIMENSION_TEXTURE2D &&
            info->array_size == 1 &&
            (info->misc_flag & DDS_RESOURCE_MISC_TEXTURECUBE) == 0;
    }
    return 1;
}

static edds_status scan_lz4(
    FILE *input,
    uint64_t offset,
    uint32_t stored_bytes,
    uint32_t *decoded_bytes,
    uint32_t *block_count,
    edds_cancelled_fn cancel,
    void *context,
    edds_error *error) {
    uint8_t word[4];
    uint64_t consumed = 0;
    uint32_t count = 0;
    uint32_t total;
    int saw_final = 0;

    if (stored_bytes < 8 || !seek_to(input, offset) || !read_exact(input, word, sizeof word)) {
        fail(error, "invalid-lz4-frame", "The LZ4 payload header is truncated.");
        return EDDS_INVALID_INPUT;
    }
    total = u32le(word);
    consumed = 4;
    if (total == 0 || total > EDDS_MAX_PREVIEW_BYTES) {
        fail(error, "decoded-size-limit", "The LZ4 decoded size %u is outside the supported limit.", total);
        return EDDS_INVALID_INPUT;
    }

    while (consumed < stored_bytes) {
        uint32_t framed;
        uint32_t compressed;
        if (cancelled(cancel, context, error)) {
            return EDDS_CANCELLED;
        }
        if (count >= EDDS_MAX_LZ4_BLOCKS || stored_bytes - consumed < 4 ||
            !read_exact(input, word, sizeof word)) {
            fail(error, "invalid-lz4-frame", "The LZ4 block table is malformed or exceeds %u blocks.", EDDS_MAX_LZ4_BLOCKS);
            return EDDS_INVALID_INPUT;
        }
        framed = u32le(word);
        compressed = framed & 0x7fffffffu;
        consumed += 4;
        if (compressed == 0 || compressed > EDDS_MAX_LZ4_STORED_BLOCK ||
            compressed > stored_bytes - consumed) {
            fail(error, "invalid-lz4-block", "An LZ4 block has an invalid stored size of %u bytes.", compressed);
            return EDDS_INVALID_INPUT;
        }
        if (fseek(input, (long)compressed, SEEK_CUR) != 0) {
            fail(error, "truncated-lz4-block", "An LZ4 block is truncated.");
            return EDDS_INVALID_INPUT;
        }
        consumed += compressed;
        ++count;
        if ((framed & 0x80000000u) != 0) {
            saw_final = 1;
            break;
        }
    }

    if (!saw_final || consumed != stored_bytes) {
        fail(error, "invalid-lz4-final-block", "The LZ4 framing has no unique final block at the payload boundary.");
        return EDDS_INVALID_INPUT;
    }
    *decoded_bytes = total;
    *block_count = count;
    return EDDS_OK;
}

edds_status edds_inspect(
    FILE *input,
    edds_info *info,
    edds_cancelled_fn cancel,
    void *cancel_context,
    edds_error *error) {
    uint8_t header[DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES];
    uint8_t descriptor[8];
    stored_mip stored[EDDS_MAX_MIPS];
    uint64_t size;
    uint64_t payload_at;
    uint32_t mip_count;
    uint32_t header_bytes = DDS_HEADER_BYTES;

    if (input == NULL || info == NULL) {
        fail(error, "invalid-api-argument", "The input stream and inspection output are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(info, 0, sizeof *info);
    if (error != NULL) {
        memset(error, 0, sizeof *error);
    }
    if (cancelled(cancel, cancel_context, error)) {
        return EDDS_CANCELLED;
    }
    if (!file_size(input, &size)) {
        fail(error, "input-io", "The EDDS input could not be measured.");
        return EDDS_INVALID_INPUT;
    }
    if (size < DDS_HEADER_BYTES + 8u || size > EDDS_MAX_FILE_BYTES) {
        fail(error, "file-size-limit", "The input size is outside the supported range through %llu bytes.",
            (unsigned long long)EDDS_MAX_FILE_BYTES);
        return EDDS_INVALID_INPUT;
    }
    if (!read_exact(input, header, DDS_HEADER_BYTES)) {
        fail(error, "truncated-dds-header", "The DDS header is truncated.");
        return EDDS_INVALID_INPUT;
    }
    if (memcmp(header, "DDS ", 4) != 0 || u32le(header + 4) != 124 ||
        u32le(header + 76) != 32 || memcmp(header + 36, "ENF1", 4) != 0) {
        fail(error, "invalid-edds-header", "The input is not a supported DDS header with an ENF1 marker.");
        return EDDS_INVALID_INPUT;
    }

    info->flags = u32le(header + 8);
    info->height = u32le(header + 12);
    info->width = u32le(header + 16);
    info->pitch_or_linear_size = u32le(header + 20);
    info->depth = u32le(header + 24);
    mip_count = u32le(header + 28);
    info->pixel_format_flags = u32le(header + 80);
    memcpy(info->four_cc, header + 84, 4);
    info->four_cc[4] = '\0';
    if (info->four_cc[0] == '\0' && info->four_cc[1] == '\0' &&
        info->four_cc[2] == '\0' && info->four_cc[3] == '\0') {
        (void)memcpy(info->four_cc, "NONE", 5);
    } else {
        for (uint32_t at = 0; at < 4; ++at) {
            const unsigned char character = (unsigned char)info->four_cc[at];
            if (character < 0x20u || character > 0x7eu) {
                info->four_cc[at] = '?';
            }
        }
    }
    info->rgb_bit_count = u32le(header + 88);
    info->r_mask = u32le(header + 92);
    info->g_mask = u32le(header + 96);
    info->b_mask = u32le(header + 100);
    info->a_mask = u32le(header + 104);
    info->caps = u32le(header + 108);
    info->caps2 = u32le(header + 112);

    if (info->width == 0 || info->height == 0 ||
        info->width > EDDS_MAX_DIMENSION || info->height > EDDS_MAX_DIMENSION) {
        fail(error, "dimension-limit", "Texture dimensions must be between 1 and %u.", EDDS_MAX_DIMENSION);
        return EDDS_INVALID_INPUT;
    }
    if (mip_count == 0) {
        mip_count = 1;
    }
    if (mip_count > EDDS_MAX_MIPS) {
        fail(error, "mip-count-limit", "The texture has more than %u mip levels.", EDDS_MAX_MIPS);
        return EDDS_INVALID_INPUT;
    }

    if ((info->pixel_format_flags & DDPF_FOURCC) != 0 &&
        strcmp(info->four_cc, "DX10") == 0) {
        if (size < DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES + (uint64_t)mip_count * 8u ||
            !read_exact(input, header + DDS_HEADER_BYTES, DDS_DX10_HEADER_BYTES)) {
            fail(error, "truncated-dx10-header", "The DDS DX10 header is truncated.");
            return EDDS_INVALID_INPUT;
        }
        info->dxgi_format = u32le(header + 128);
        info->resource_dimension = u32le(header + 132);
        info->misc_flag = u32le(header + 136);
        info->array_size = u32le(header + 140);
        header_bytes += DDS_DX10_HEADER_BYTES;
    }

    info->mip_count = mip_count;
    info->header_bytes = header_bytes;
    info->pixel_format = classify(info);
    {
        uint32_t previewed_bytes = 0;
        info->preview_supported =
            edds_gpu_mip_bytes(info->pixel_format, info->width, info->height) != 0 &&
            expected_rgba_bytes(info->width, info->height, &previewed_bytes) &&
            topology_is_previewable(info);
    }

    if (size < (uint64_t)header_bytes + (uint64_t)mip_count * 8u) {
        fail(error, "truncated-mip-table", "The ENF1 mip table is truncated.");
        return EDDS_INVALID_INPUT;
    }
    payload_at = (uint64_t)header_bytes + (uint64_t)mip_count * 8u;
    if (!seek_to(input, header_bytes)) {
        fail(error, "input-io", "The ENF1 mip table could not be read.");
        return EDDS_INVALID_INPUT;
    }

    for (uint32_t stored_index = 0; stored_index < mip_count; ++stored_index) {
        uint32_t level = mip_count - stored_index - 1u;
        uint32_t expected = 0;
        edds_status status;
        if (cancelled(cancel, cancel_context, error)) {
            return EDDS_CANCELLED;
        }
        if (!read_exact(input, descriptor, sizeof descriptor)) {
            fail(error, "truncated-mip-table", "The ENF1 mip table is truncated.");
            return EDDS_INVALID_INPUT;
        }
        if (memcmp(descriptor, "COPY", 4) == 0) {
            stored[stored_index].container = EDDS_CONTAINER_COPY;
        } else if (memcmp(descriptor, "LZ4 ", 4) == 0) {
            stored[stored_index].container = EDDS_CONTAINER_LZ4;
        } else {
            fail(error, "unsupported-container", "Mip %u has an unknown ENF1 container tag.", level);
            return EDDS_INVALID_INPUT;
        }
        stored[stored_index].stored_bytes = u32le(descriptor + 4);
        stored[stored_index].data_offset = payload_at;
        stored[stored_index].block_count = 0;
        if (stored[stored_index].stored_bytes == 0 ||
            stored[stored_index].stored_bytes > size - payload_at) {
            fail(error, "truncated-mip-payload", "Mip %u extends beyond the input boundary.", level);
            return EDDS_INVALID_INPUT;
        }
        if (stored[stored_index].container == EDDS_CONTAINER_LZ4) {
            const long table_return = (long)(header_bytes + (stored_index + 1u) * 8u);
            status = scan_lz4(input, payload_at, stored[stored_index].stored_bytes,
                &stored[stored_index].decoded_bytes, &stored[stored_index].block_count,
                cancel, cancel_context, error);
            if (status != EDDS_OK) {
                return status;
            }
            if (fseek(input, table_return, SEEK_SET) != 0) {
                fail(error, "input-io", "The ENF1 mip table could not be resumed.");
                return EDDS_INVALID_INPUT;
            }
        } else {
            stored[stored_index].decoded_bytes = stored[stored_index].stored_bytes;
        }
        if (info->preview_supported) {
            expected = edds_gpu_mip_bytes(info->pixel_format, mip_dimension(info->width, level),
                mip_dimension(info->height, level));
            if (expected == 0 || stored[stored_index].decoded_bytes != expected) {
                fail(error, "unexpected-mip-size", "Mip %u decodes to %u bytes; its dimensions require %u.",
                    level, stored[stored_index].decoded_bytes, expected);
                return EDDS_INVALID_INPUT;
            }
        }
        payload_at += stored[stored_index].stored_bytes;
    }

    if (payload_at != size) {
        fail(error, "trailing-data", "The EDDS has %llu unexpected byte(s) after its mip payloads.",
            (unsigned long long)(size - payload_at));
        return EDDS_INVALID_INPUT;
    }

    for (uint32_t level = 0; level < mip_count; ++level) {
        const stored_mip *source = &stored[mip_count - level - 1u];
        edds_mip *destination = &info->mips[level];
        destination->level = level;
        destination->width = mip_dimension(info->width, level);
        destination->height = mip_dimension(info->height, level);
        destination->container = source->container;
        destination->stored_bytes = source->stored_bytes;
        destination->decoded_bytes = source->decoded_bytes;
        destination->block_count = source->block_count;
        destination->data_offset = source->data_offset;
    }
    return EDDS_OK;
}

static int extend_length(
    const uint8_t *source,
    size_t source_size,
    size_t *source_at,
    size_t *length) {
    uint8_t addition;
    do {
        if (*source_at >= source_size) {
            return 0;
        }
        addition = source[(*source_at)++];
        if (*length > SIZE_MAX - addition) {
            return 0;
        }
        *length += addition;
    } while (addition == 255);
    return 1;
}

static int decode_lz4_block(
    const uint8_t *source,
    size_t source_size,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_at) {
    size_t source_at = 0;
    while (source_at < source_size) {
        const uint8_t token = source[source_at++];
        size_t literal_length = token >> 4;
        size_t match_length;
        size_t offset;
        if (literal_length == 15 && !extend_length(source, source_size, &source_at, &literal_length)) {
            return 0;
        }
        if (literal_length > source_size - source_at || literal_length > output_capacity - *output_at) {
            return 0;
        }
        memcpy(output + *output_at, source + source_at, literal_length);
        source_at += literal_length;
        *output_at += literal_length;
        if (source_at == source_size) {
            return 1;
        }
        if (source_size - source_at < 2) {
            return 0;
        }
        offset = (size_t)source[source_at] | ((size_t)source[source_at + 1] << 8);
        source_at += 2;
        if (offset == 0 || offset > *output_at) {
            return 0;
        }
        match_length = (token & 0x0fu) + 4u;
        if ((token & 0x0fu) == 15 && !extend_length(source, source_size, &source_at, &match_length)) {
            return 0;
        }
        if (match_length > output_capacity - *output_at) {
            return 0;
        }
        for (size_t index = 0; index < match_length; ++index) {
            output[*output_at] = output[*output_at - offset];
            ++*output_at;
        }
    }
    return 1;
}

static edds_status decode_lz4(
    FILE *input,
    const edds_mip *mip,
    edds_cancelled_fn cancel,
    void *context,
    uint8_t *output,
    edds_error *error) {
    uint8_t word[4];
    uint64_t consumed = 4;
    size_t output_at = 0;
    int saw_final = 0;
    if (!seek_to(input, mip->data_offset) || !read_exact(input, word, sizeof word) ||
        u32le(word) != mip->decoded_bytes) {
        fail(error, "changed-input", "The LZ4 payload no longer matches its inspection.");
        return EDDS_INVALID_INPUT;
    }
    while (consumed < mip->stored_bytes) {
        uint32_t framed;
        uint32_t compressed_size;
        uint8_t *compressed;
        size_t block_output_at;
        if (cancelled(cancel, context, error)) {
            return EDDS_CANCELLED;
        }
        if (!read_exact(input, word, sizeof word)) {
            fail(error, "truncated-lz4-block", "An LZ4 block header is truncated.");
            return EDDS_INVALID_INPUT;
        }
        framed = u32le(word);
        compressed_size = framed & 0x7fffffffu;
        consumed += 4;
        if (compressed_size == 0 || compressed_size > EDDS_MAX_LZ4_STORED_BLOCK ||
            compressed_size > mip->stored_bytes - consumed) {
            fail(error, "changed-input", "An LZ4 block no longer matches its inspection.");
            return EDDS_INVALID_INPUT;
        }
        compressed = edds_alloc(compressed_size);
        if (compressed == NULL) {
            fail(error, "allocation-failed", "Memory for an LZ4 block could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
        block_output_at = output_at;
        if (!read_exact(input, compressed, compressed_size) ||
            !decode_lz4_block(compressed, compressed_size, output, mip->decoded_bytes, &output_at)) {
            edds_free(compressed);
            fail(error, "invalid-lz4-data", "An LZ4 block cannot be decoded within the declared boundary.");
            return EDDS_INVALID_INPUT;
        }
        edds_free(compressed);
        consumed += compressed_size;
        if ((framed & 0x80000000u) == 0 &&
            output_at - block_output_at != LZ4_DECODED_BLOCK_BYTES) {
            fail(error, "invalid-lz4-block-size", "A non-final LZ4 block did not decode to 65536 bytes.");
            return EDDS_INVALID_INPUT;
        }
        if ((framed & 0x80000000u) != 0) {
            saw_final = 1;
            break;
        }
    }
    if (!saw_final || consumed != mip->stored_bytes || output_at != mip->decoded_bytes) {
        fail(error, "invalid-lz4-output", "The LZ4 stream does not produce its declared output size.");
        return EDDS_INVALID_INPUT;
    }
    return EDDS_OK;
}

edds_status edds_preview(
    FILE *input,
    const edds_info *info,
    uint32_t level,
    edds_cancelled_fn cancel,
    void *cancel_context,
    uint8_t **rgba,
    size_t *rgba_size,
    edds_error *error) {
    const edds_mip *mip;
    uint8_t *raw;
    uint8_t *pixels;
    uint32_t decoded_bytes = 0;
    edds_status status = EDDS_OK;
    if (rgba != NULL) {
        *rgba = NULL;
    }
    if (rgba_size != NULL) {
        *rgba_size = 0;
    }
    if (input == NULL || info == NULL || rgba == NULL || rgba_size == NULL) {
        fail(error, "invalid-api-argument", "The input, inspection, and preview outputs are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (level >= info->mip_count) {
        fail(error, "invalid-mip", "Mip level %u does not exist.", level);
        return EDDS_INVALID_INPUT;
    }
    if (!info->preview_supported) {
        fail(error, "unsupported-pixel-format", "This DDS pixel format or texture topology cannot be previewed.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (cancelled(cancel, cancel_context, error)) {
        return EDDS_CANCELLED;
    }
    mip = &info->mips[level];
    if (!expected_rgba_bytes(mip->width, mip->height, &decoded_bytes)) {
        fail(error, "decoded-size-limit", "Mip %u decodes to more pixels than one preview holds.", level);
        return EDDS_INVALID_INPUT;
    }
    raw = edds_alloc(mip->decoded_bytes);
    pixels = edds_alloc(decoded_bytes);
    if (raw == NULL || pixels == NULL) {
        edds_free(raw);
        edds_free(pixels);
        fail(error, "allocation-failed", "Memory for the selected mip could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (mip->container == EDDS_CONTAINER_COPY) {
        if (!seek_to(input, mip->data_offset) || !read_exact(input, raw, mip->decoded_bytes)) {
            fail(error, "changed-input", "The COPY payload no longer matches its inspection.");
            status = EDDS_INVALID_INPUT;
        }
    } else {
        status = decode_lz4(input, mip, cancel, cancel_context, raw, error);
    }
    if (status == EDDS_OK) {
        if (!edds_gpu_decode(info->pixel_format, raw, mip->decoded_bytes,
                mip->width, mip->height, pixels)) {
            fail(error, "invalid-gpu-payload",
                "Mip %u does not hold whole %s blocks for its dimensions.",
                level, edds_pixel_format_name(info->pixel_format));
            status = EDDS_INVALID_INPUT;
        } else {
            *rgba = pixels;
            *rgba_size = decoded_bytes;
            pixels = NULL;
        }
    }
    edds_free(raw);
    edds_free(pixels);
    return status;
}

const char *edds_container_name(edds_container container) {
    return container == EDDS_CONTAINER_COPY ? "COPY" : "LZ4";
}

const char *edds_status_category(edds_status status) {
    switch (status) {
        case EDDS_INVALID_INVOCATION: return "invalid-invocation";
        case EDDS_INVALID_INPUT: return "invalid-input";
        case EDDS_UNSUPPORTED_FORMAT: return "unsupported-format";
        case EDDS_CANCELLED: return "cancelled";
        case EDDS_INTERNAL_FAILURE: return "internal-failure";
        case EDDS_OK: return "success";
        default: return "internal-failure";
    }
}
