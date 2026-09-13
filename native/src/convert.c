#include "image.h"
#include "gpu.h"

#include <stdlib.h>
#include <string.h>

enum {
    DDS_HEADER_BYTES = 128,
    DDS_DX10_HEADER_BYTES = 20,
    DDS_RESOURCE_DIMENSION_TEXTURE2D = 3,
    DDSD_CAPS = 0x00000001,
    DDSD_HEIGHT = 0x00000002,
    DDSD_WIDTH = 0x00000004,
    DDSD_PITCH = 0x00000008,
    DDSD_PIXELFORMAT = 0x00001000,
    DDSD_MIPMAPCOUNT = 0x00020000,
    DDSD_LINEARSIZE = 0x00080000,
    DDPF_ALPHAPIXELS = 0x00000001,
    DDPF_FOURCC = 0x00000004,
    DDPF_RGB = 0x00000040,
    DDSCAPS_COMPLEX = 0x00000008,
    DDSCAPS_TEXTURE = 0x00001000,
    DDSCAPS_MIPMAP = 0x00400000,
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R8_UNORM = 61,
    DXGI_FORMAT_BC4_UNORM = 80,
    DXGI_FORMAT_BC5_UNORM = 83,
    DXGI_FORMAT_BC7_UNORM = 98
};

typedef struct generated_mip {
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint8_t *bgra;
    /** The same mip in the runtime format, which is what the container then compresses. */
    uint32_t payload_bytes;
    uint8_t *payload;
    edds_container container;
    uint32_t stored_bytes;
    uint8_t *stored;
} generated_mip;

static uint32_t mip_count(uint32_t width, uint32_t height, int generate) {
    uint32_t count = 1;
    if (!generate) {
        return count;
    }
    while (width > 1 || height > 1) {
        width = width > 1 ? width / 2u : 1u;
        height = height > 1 ? height / 2u : 1u;
        ++count;
    }
    return count;
}

static int mip_bytes(uint32_t width, uint32_t height, uint32_t *bytes) {
    const uint64_t value = (uint64_t)width * height * 4u;
    if (value == 0 || value > EDDS_MAX_PREVIEW_BYTES) {
        return 0;
    }
    *bytes = (uint32_t)value;
    return 1;
}

static void source_mip(const edds_decoded_source *source, generated_mip *mip) {
    for (size_t at = 0; at < mip->bytes; at += 4u) {
        mip->bgra[at] = source->rgba[at + 2u];
        mip->bgra[at + 1u] = source->rgba[at + 1u];
        mip->bgra[at + 2u] = source->rgba[at];
        mip->bgra[at + 3u] = source->has_alpha ? source->rgba[at + 3u] : 255u;
    }
}

static void box_mip(const generated_mip *previous, generated_mip *next) {
    const uint32_t sample_width = previous->width > 1 ? 2u : 1u;
    const uint32_t sample_height = previous->height > 1 ? 2u : 1u;
    const uint32_t divisor = sample_width * sample_height;
    for (uint32_t y = 0; y < next->height; ++y) {
        for (uint32_t x = 0; x < next->width; ++x) {
            const size_t output_at = ((size_t)y * next->width + x) * 4u;
            for (uint32_t channel = 0; channel < 4; ++channel) {
                uint32_t total = 0;
                for (uint32_t dy = 0; dy < sample_height; ++dy) {
                    for (uint32_t dx = 0; dx < sample_width; ++dx) {
                        const size_t source_at =
                            ((size_t)(y * sample_height + dy) * previous->width +
                                x * sample_width + dx) * 4u;
                        total += previous->bgra[source_at + channel];
                    }
                }
                next->bgra[output_at + channel] = (uint8_t)((total + divisor / 2u) / divisor);
            }
        }
    }
}

static uint32_t lz4_hash(const uint8_t *at) {
    const uint32_t value = (uint32_t)at[0] |
        ((uint32_t)at[1] << 8) |
        ((uint32_t)at[2] << 16) |
        ((uint32_t)at[3] << 24);
    return (value * 2654435761u) >> 16;
}

static int write_lz4_length(uint8_t *output, size_t capacity, size_t *at, size_t length) {
    while (length >= 255u) {
        if (*at >= capacity) return 0;
        output[(*at)++] = 255;
        length -= 255u;
    }
    if (*at >= capacity) return 0;
    output[(*at)++] = (uint8_t)length;
    return 1;
}

static int lz4_block(
    const uint8_t *input,
    size_t size,
    unsigned search_depth,
    uint8_t *output,
    size_t capacity,
    size_t *written
) {
    int32_t *head = malloc(65536u * sizeof *head);
    int32_t *previous = malloc((size == 0 ? 1u : size) * sizeof *previous);
    size_t input_at = 0;
    size_t anchor = 0;
    size_t output_at = 0;
    int ok = 0;
    if (head == NULL || previous == NULL) {
        goto done;
    }
    memset(head, 0xff, 65536u * sizeof *head);
    while (input_at + 12u <= size) {
        const uint32_t hash = lz4_hash(input + input_at);
        int32_t candidate = head[hash];
        size_t best_length = 0;
        size_t best_offset = 0;
        unsigned searched = 0;
        previous[input_at] = candidate;
        head[hash] = (int32_t)input_at;
        while (candidate >= 0 && searched++ < search_depth &&
               input_at - (size_t)candidate <= 65535u) {
            size_t length = 0;
            const size_t maximum = size - input_at - 5u;
            while (length < maximum && input[(size_t)candidate + length] == input[input_at + length]) {
                ++length;
            }
            if (length > best_length && length >= 4u) {
                best_length = length;
                best_offset = input_at - (size_t)candidate;
                if (search_depth == 1u || length == maximum) break;
            }
            candidate = previous[candidate];
        }
        if (best_length < 4u) {
            ++input_at;
            continue;
        }
        {
            const size_t literals = input_at - anchor;
            const size_t match_code = best_length - 4u;
            const size_t token_at = output_at++;
            if (token_at >= capacity) goto done;
            output[token_at] = (uint8_t)((literals < 15u ? literals : 15u) << 4);
            if (literals >= 15u &&
                !write_lz4_length(output, capacity, &output_at, literals - 15u)) goto done;
            if (literals > capacity - output_at) goto done;
            memcpy(output + output_at, input + anchor, literals);
            output_at += literals;
            if (capacity - output_at < 2u) goto done;
            output[output_at++] = (uint8_t)best_offset;
            output[output_at++] = (uint8_t)(best_offset >> 8);
            output[token_at] |= (uint8_t)(match_code < 15u ? match_code : 15u);
            if (match_code >= 15u &&
                !write_lz4_length(output, capacity, &output_at, match_code - 15u)) goto done;
        }
        for (size_t index = 1; index < best_length && input_at + index + 4u <= size; ++index) {
            const size_t position = input_at + index;
            const uint32_t inserted_hash = lz4_hash(input + position);
            previous[position] = head[inserted_hash];
            head[inserted_hash] = (int32_t)position;
        }
        input_at += best_length;
        anchor = input_at;
    }
    {
        const size_t literals = size - anchor;
        const size_t token_at = output_at++;
        if (token_at >= capacity) goto done;
        output[token_at] = (uint8_t)((literals < 15u ? literals : 15u) << 4);
        if (literals >= 15u &&
            !write_lz4_length(output, capacity, &output_at, literals - 15u)) goto done;
        if (literals > capacity - output_at) goto done;
        memcpy(output + output_at, input + anchor, literals);
        output_at += literals;
    }
    *written = output_at;
    ok = 1;

done:
    free(head);
    free(previous);
    return ok;
}

static uint8_t *lz4_frame(
    const uint8_t *input,
    uint32_t size,
    edds_format_compress mode,
    uint32_t *stored_bytes
) {
    const uint32_t block_count = (size + 65535u) / 65536u;
    const size_t capacity = 4u + (size_t)block_count * (4u + 16u) + size + size / 255u;
    const unsigned depth = mode == EDDS_COMPRESS_FASTEST ? 1u :
        (mode == EDDS_COMPRESS_MEDIUM ? 16u : 64u);
    uint8_t *frame = malloc(capacity);
    size_t output_at = 4;
    uint32_t input_at = 0;
    if (frame == NULL) {
        return NULL;
    }
    edds_put_u32le(frame, size);
    for (uint32_t block = 0; block < block_count; ++block) {
        const uint32_t block_bytes = size - input_at > 65536u ? 65536u : size - input_at;
        const size_t descriptor_at = output_at;
        size_t compressed_size = 0;
        output_at += 4;
        if (!lz4_block(input + input_at, block_bytes, depth, frame + output_at,
                capacity - output_at, &compressed_size) || compressed_size > UINT32_MAX) {
            free(frame);
            return NULL;
        }
        edds_put_u32le(frame + descriptor_at, (block + 1u == block_count ? 0x80000000u : 0u) |
            (uint32_t)compressed_size);
        output_at += compressed_size;
        input_at += block_bytes;
    }
    if (output_at > UINT32_MAX) {
        free(frame);
        return NULL;
    }
    *stored_bytes = (uint32_t)output_at;
    return frame;
}

static void report(edds_progress_fn progress, void *context, double value) {
    if (progress != NULL) progress(context, value);
}

static edds_status prepare_storage(
    generated_mip *mips,
    uint32_t count,
    const edds_profile *profile,
    edds_progress_fn progress,
    void *progress_context,
    edds_error *error
) {
    for (uint32_t at = 0; at < count; ++at) {
        generated_mip *mip = &mips[at];
        /* Container compression is the long part of a conversion, so it moves the row per mip. */
        report(progress, progress_context, 0.45 + 0.45 * ((double)at / (double)count));
        mip->container = EDDS_CONTAINER_COPY;
        mip->stored_bytes = mip->payload_bytes;
        mip->stored = mip->payload;
        if (profile->format_compress != EDDS_COMPRESS_COPY) {
            uint32_t compressed_bytes = 0;
            uint8_t *compressed = lz4_frame(mip->payload, mip->payload_bytes,
                profile->format_compress, &compressed_bytes);
            if (compressed == NULL) {
                edds_fail(error, "allocation-failed", "Memory for LZ4 container compression could not be allocated.");
                return EDDS_INTERNAL_FAILURE;
            }
            if ((uint64_t)compressed_bytes * 100u <=
                (uint64_t)mip->payload_bytes * profile->compress_threshold) {
                mip->container = EDDS_CONTAINER_LZ4;
                mip->stored_bytes = compressed_bytes;
                mip->stored = compressed;
            } else {
                free(compressed);
            }
        }
    }
    return EDDS_OK;
}

static void free_mips(generated_mip *mips, uint32_t count) {
    for (uint32_t at = 0; at < count; ++at) {
        if (mips[at].stored != mips[at].payload) {
            free(mips[at].stored);
        }
        free(mips[at].payload);
        free(mips[at].bgra);
        mips[at].bgra = NULL;
        mips[at].payload = NULL;
        mips[at].stored = NULL;
    }
}

/**
 * Turns the staged BGRA chain into the runtime format. Container compression happens afterwards
 * and over these bytes, so `FormatCompress` cannot change a single decoded pixel of the result.
 */
static edds_status encode_mips(
    generated_mip *mips,
    uint32_t count,
    edds_pixel_format format,
    uint32_t quality,
    edds_cancelled_fn cancelled,
    void *context,
    edds_error *error
) {
    for (uint32_t at = 0; at < count; ++at) {
        generated_mip *mip = &mips[at];
        mip->payload_bytes = edds_gpu_mip_bytes(format, mip->width, mip->height);
        if (mip->payload_bytes == 0) {
            edds_fail(error, "mip-size-limit", "A generated mip exceeds the runtime-format limit.");
            return EDDS_INVALID_INPUT;
        }
        mip->payload = malloc(mip->payload_bytes);
        if (mip->payload == NULL) {
            edds_fail(error, "allocation-failed", "Memory for the runtime-format mip could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
        if (cancelled != NULL && cancelled(context)) {
            edds_fail(error, "cancelled", "The conversion was cancelled.");
            return EDDS_CANCELLED;
        }
        edds_gpu_encode(format, quality, mip->bgra, mip->width, mip->height, mip->payload);
    }
    return EDDS_OK;
}

static edds_status generate_mips(
    const edds_decoded_source *source,
    const edds_profile *profile,
    generated_mip *mips,
    uint32_t *count,
    edds_cancelled_fn cancelled,
    void *context,
    edds_error *error
) {
    *count = mip_count(source->width, source->height, profile->generate_mips);
    memset(mips, 0, sizeof(*mips) * *count);
    mips[0].width = source->width;
    mips[0].height = source->height;
    for (uint32_t at = 0; at < *count; ++at) {
        if (cancelled != NULL && cancelled(context)) {
            free_mips(mips, *count);
            edds_fail(error, "cancelled", "The conversion was cancelled.");
            return EDDS_CANCELLED;
        }
        if (at > 0) {
            mips[at].width = mips[at - 1u].width > 1 ? mips[at - 1u].width / 2u : 1u;
            mips[at].height = mips[at - 1u].height > 1 ? mips[at - 1u].height / 2u : 1u;
        }
        if (!mip_bytes(mips[at].width, mips[at].height, &mips[at].bytes)) {
            free_mips(mips, *count);
            edds_fail(error, "mip-size-limit", "A generated mip exceeds the decoded-image limit.");
            return EDDS_INVALID_INPUT;
        }
        mips[at].bgra = malloc(mips[at].bytes);
        if (mips[at].bgra == NULL) {
            free_mips(mips, *count);
            edds_fail(error, "allocation-failed", "Memory for the mip chain could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
        if (at == 0) {
            source_mip(source, &mips[at]);
        } else {
            box_mip(&mips[at - 1u], &mips[at]);
        }
    }
    return EDDS_OK;
}

/** The DXGI format a DX10 header names, or 0 for a format that has a legacy descriptor. */
static uint32_t dxgi_format_of(edds_pixel_format format) {
    switch (format) {
        case EDDS_PIXEL_R8: return DXGI_FORMAT_R8_UNORM;
        case EDDS_PIXEL_RG8: return DXGI_FORMAT_R8G8_UNORM;
        case EDDS_PIXEL_BC4: return DXGI_FORMAT_BC4_UNORM;
        case EDDS_PIXEL_BC5: return DXGI_FORMAT_BC5_UNORM;
        case EDDS_PIXEL_BC7: return DXGI_FORMAT_BC7_UNORM;
        default: return 0;
    }
}

/**
 * Writes the DDS header the way DayZ's own textures carry it: a block format declares its top
 * mip as a linear size, an uncompressed one declares a pitch, BGRA/BGRX and the two DXT formats
 * keep their legacy descriptors, and everything else names its DXGI format through a DX10 header.
 */
static uint32_t dds_header(
    uint8_t header[DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES],
    edds_pixel_format format,
    uint32_t width,
    uint32_t height,
    uint32_t count,
    uint32_t top_mip_bytes
) {
    const uint32_t block = edds_gpu_block_bytes(format);
    const uint32_t dxgi = dxgi_format_of(format);
    const uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT |
        DDSD_MIPMAPCOUNT | (block != 0 ? DDSD_LINEARSIZE : DDSD_PITCH);
    memset(header, 0, DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES);
    memcpy(header, "DDS ", 4);
    edds_put_u32le(header + 4, 124);
    edds_put_u32le(header + 8, flags);
    edds_put_u32le(header + 12, height);
    edds_put_u32le(header + 16, width);
    edds_put_u32le(header + 20,
        block != 0 ? top_mip_bytes : width * edds_gpu_pixel_bytes(format));
    edds_put_u32le(header + 28, count);
    memcpy(header + 36, "ENF1", 4);
    edds_put_u32le(header + 76, 32);
    edds_put_u32le(header + 108, DDSCAPS_TEXTURE |
        (count > 1 ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0u));
    if (format == EDDS_PIXEL_BGRA8 || format == EDDS_PIXEL_BGRX8) {
        const int alpha = format == EDDS_PIXEL_BGRA8;
        edds_put_u32le(header + 80, DDPF_RGB | (alpha ? DDPF_ALPHAPIXELS : 0u));
        edds_put_u32le(header + 88, 32);
        edds_put_u32le(header + 92, 0x00ff0000u);
        edds_put_u32le(header + 96, 0x0000ff00u);
        edds_put_u32le(header + 100, 0x000000ffu);
        edds_put_u32le(header + 104, alpha ? 0xff000000u : 0u);
        return DDS_HEADER_BYTES;
    }
    edds_put_u32le(header + 80, DDPF_FOURCC);
    if (dxgi == 0) {
        memcpy(header + 84, format == EDDS_PIXEL_DXT1 ? "DXT1" : "DXT5", 4);
        return DDS_HEADER_BYTES;
    }
    memcpy(header + 84, "DX10", 4);
    edds_put_u32le(header + 128, dxgi);
    edds_put_u32le(header + 132, DDS_RESOURCE_DIMENSION_TEXTURE2D);
    edds_put_u32le(header + 136, 0);
    edds_put_u32le(header + 140, 1);
    edds_put_u32le(header + 144, 0);
    return DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES;
}

static edds_status write_edds(
    FILE *output,
    edds_pixel_format format,
    const generated_mip *mips,
    uint32_t count,
    edds_error *error
) {
    uint8_t header[DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES];
    uint8_t descriptor[8];
    const uint32_t header_bytes =
        dds_header(header, format, mips[0].width, mips[0].height, count, mips[0].payload_bytes);
    if (fwrite(header, 1, header_bytes, output) != header_bytes) {
        goto failure;
    }
    for (uint32_t stored = 0; stored < count; ++stored) {
        const generated_mip *mip = &mips[count - stored - 1u];
        memcpy(descriptor, mip->container == EDDS_CONTAINER_COPY ? "COPY" : "LZ4 ", 4);
        edds_put_u32le(descriptor + 4, mip->stored_bytes);
        if (fwrite(descriptor, 1, sizeof descriptor, output) != sizeof descriptor) {
            goto failure;
        }
    }
    for (uint32_t stored = 0; stored < count; ++stored) {
        const generated_mip *mip = &mips[count - stored - 1u];
        if (fwrite(mip->stored, 1, mip->stored_bytes, output) != mip->stored_bytes) {
            goto failure;
        }
    }
    if (fflush(output) != 0) {
        goto failure;
    }
    return EDDS_OK;

failure:
    edds_fail(error, "output-write-failed", "The EDDS output could not be written completely.");
    return EDDS_INTERNAL_FAILURE;
}

void edds_default_profile(edds_profile *profile) {
    if (profile != NULL) {
        profile->format_compress = EDDS_COMPRESS_FASTEST;
        profile->compress_threshold = 80;
        profile->conversion = EDDS_CONVERSION_NONE;
        profile->conversion_quality = EDDS_QUALITY_SCALE;
        profile->generate_mips = 1;
    }
}

edds_status edds_profile_check(const edds_profile *profile, edds_error *error) {
    const edds_conversion_capability *conversion;
    if (profile == NULL) {
        edds_fail(error, "invalid-api-argument", "A conversion profile is required.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (profile->format_compress < EDDS_COMPRESS_COPY ||
        profile->format_compress > EDDS_COMPRESS_BEST || profile->compress_threshold > 100u ||
        (profile->generate_mips != 0 && profile->generate_mips != 1)) {
        edds_fail(error, "unsupported-setting",
            "The conversion profile is outside the supported Workbench slice.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    conversion = edds_conversion_capability_of(profile->conversion);
    if (conversion == NULL || !conversion->supported) {
        edds_fail(error, "unsupported-setting",
            "Workbench setting Conversion=%s is recognized but unsupported.",
            conversion == NULL ? "unknown" : conversion->workbench_name);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (profile->conversion_quality > EDDS_QUALITY_SCALE) {
        edds_fail(error, "unsupported-setting",
            "Workbench setting ConversionQuality must be between 0 and 1.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    /*
     * Workbench itself calls the field "Conversion quality for compressed formats", so a quality
     * other than the default alongside an uncompressed conversion is a combination nothing has
     * confirmed the meaning of. It is refused rather than accepted and quietly ignored.
     */
    if (!conversion->uses_quality && profile->conversion_quality != EDDS_QUALITY_SCALE) {
        edds_fail(error, "unsupported-setting",
            "Workbench setting ConversionQuality has no confirmed effect on Conversion=%s.",
            conversion->workbench_name);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    return EDDS_OK;
}

/**
 * Two conversions branch on alpha, and they weigh it differently. `None` keeps whichever channel
 * layout the source declared, because BGRX and BGRA cost the same four bytes and discarding a
 * declared channel would be a change the source did not ask for. `DXTCompression` branches on
 * whether the alpha is *used*: BC3 is twice the size of BC1 and spends all of it on an alpha block,
 * so a fully opaque channel buys nothing. Only a sample below 255 is alpha the result has to carry.
 */
edds_pixel_format edds_profile_pixel_format(const edds_profile *profile, edds_source_alpha alpha) {
    switch (profile == NULL ? EDDS_CONVERSION_NONE : profile->conversion) {
        case EDDS_CONVERSION_NONE:
            return alpha == EDDS_ALPHA_ABSENT ? EDDS_PIXEL_BGRX8 : EDDS_PIXEL_BGRA8;
        case EDDS_CONVERSION_DXT:
            return alpha == EDDS_ALPHA_USED ? EDDS_PIXEL_DXT5 : EDDS_PIXEL_DXT1;
        case EDDS_CONVERSION_RED: return EDDS_PIXEL_R8;
        case EDDS_CONVERSION_RED_HQ: return EDDS_PIXEL_BC4;
        case EDDS_CONVERSION_RED_GREEN: return EDDS_PIXEL_RG8;
        case EDDS_CONVERSION_RED_GREEN_HQ: return EDDS_PIXEL_BC5;
        case EDDS_CONVERSION_COLOR_HQ: return EDDS_PIXEL_BC7;
        default: return EDDS_PIXEL_UNKNOWN;
    }
}

/** Read off this one source's own samples, never off a batch and never off a file name. */
static edds_source_alpha source_alpha_of(const edds_decoded_source *source) {
    if (!source->has_alpha) {
        return EDDS_ALPHA_ABSENT;
    }
    for (size_t at = 3; at < (size_t)source->width * source->height * 4u; at += 4u) {
        if (source->rgba[at] != 255u) {
            return EDDS_ALPHA_USED;
        }
    }
    return EDDS_ALPHA_OPAQUE;
}

edds_status edds_convert(
    FILE *source,
    edds_source_format source_format,
    FILE *output,
    const edds_profile *profile,
    edds_cancelled_fn cancelled,
    void *cancel_context,
    edds_progress_fn progress,
    void *progress_context,
    edds_error *error
) {
    edds_decoded_source image = { 0, 0, 0, NULL };
    generated_mip mips[EDDS_MAX_MIPS];
    edds_pixel_format format;
    uint32_t count = 0;
    edds_status status;
    if (source == NULL || output == NULL || profile == NULL) {
        edds_fail(error, "invalid-api-argument", "The source, output, and profile are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    status = edds_profile_check(profile, error);
    if (status != EDDS_OK) {
        return status;
    }
    if (edds_source_capability_of_format(source_format) == NULL) {
        edds_fail(error, "unsupported-source-format",
            "The source format is outside the supported Workbench resource classes.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    /*
     * Deliberately without a default: the capability table above has already refused anything
     * outside the enum, so a format added to the contract with no decoder behind it is a build
     * error here rather than a run that refuses what the rest of the product advertises.
     */
    status = EDDS_UNSUPPORTED_FORMAT;
    switch (source_format) {
        case EDDS_SOURCE_PNG: status = edds_decode_png(source, &image, error); break;
        case EDDS_SOURCE_TGA: status = edds_decode_tga(source, &image, error); break;
        case EDDS_SOURCE_JPG: status = edds_decode_jpeg(source, &image, error); break;
        case EDDS_SOURCE_TIFF: status = edds_decode_tiff(source, &image, error); break;
    }
    if (status != EDDS_OK) {
        return status;
    }
    format = edds_profile_pixel_format(profile, source_alpha_of(&image));
    report(progress, progress_context, 0.15);
    status = generate_mips(&image, profile, mips, &count, cancelled, cancel_context, error);
    if (status == EDDS_OK) {
        report(progress, progress_context, 0.30);
        status = encode_mips(mips, count, format, profile->conversion_quality,
            cancelled, cancel_context, error);
    }
    if (status == EDDS_OK) {
        report(progress, progress_context, 0.45);
        status = prepare_storage(mips, count, profile, progress, progress_context, error);
    }
    if (status == EDDS_OK) {
        report(progress, progress_context, 0.90);
        status = write_edds(output, format, mips, count, error);
    }
    free_mips(mips, count);
    free(image.rgba);
    return status;
}
