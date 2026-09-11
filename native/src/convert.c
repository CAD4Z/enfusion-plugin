#include "image.h"

#include <stdlib.h>
#include <string.h>

enum {
    DDS_HEADER_BYTES = 128,
    DDSD_CAPS = 0x00000001,
    DDSD_HEIGHT = 0x00000002,
    DDSD_WIDTH = 0x00000004,
    DDSD_PITCH = 0x00000008,
    DDSD_PIXELFORMAT = 0x00001000,
    DDSD_MIPMAPCOUNT = 0x00020000,
    DDPF_ALPHAPIXELS = 0x00000001,
    DDPF_RGB = 0x00000040,
    DDSCAPS_COMPLEX = 0x00000008,
    DDSCAPS_TEXTURE = 0x00001000,
    DDSCAPS_MIPMAP = 0x00400000
};

typedef struct generated_mip {
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint8_t *bgra;
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
        mip->stored_bytes = mip->bytes;
        mip->stored = mip->bgra;
        if (profile->format_compress != EDDS_COMPRESS_COPY) {
            uint32_t compressed_bytes = 0;
            uint8_t *compressed = lz4_frame(mip->bgra, mip->bytes,
                profile->format_compress, &compressed_bytes);
            if (compressed == NULL) {
                edds_fail(error, "allocation-failed", "Memory for LZ4 container compression could not be allocated.");
                return EDDS_INTERNAL_FAILURE;
            }
            if ((uint64_t)compressed_bytes * 100u <=
                (uint64_t)mip->bytes * profile->compress_threshold) {
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
        if (mips[at].stored != mips[at].bgra) {
            free(mips[at].stored);
        }
        free(mips[at].bgra);
        mips[at].bgra = NULL;
        mips[at].stored = NULL;
    }
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

static void dds_header(uint8_t header[DDS_HEADER_BYTES], const edds_decoded_source *source, uint32_t count) {
    const uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH |
        DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT;
    memset(header, 0, DDS_HEADER_BYTES);
    memcpy(header, "DDS ", 4);
    edds_put_u32le(header + 4, 124);
    edds_put_u32le(header + 8, flags);
    edds_put_u32le(header + 12, source->height);
    edds_put_u32le(header + 16, source->width);
    edds_put_u32le(header + 20, source->width * 4u);
    edds_put_u32le(header + 28, count);
    memcpy(header + 36, "ENF1", 4);
    edds_put_u32le(header + 76, 32);
    edds_put_u32le(header + 80, DDPF_RGB | (source->has_alpha ? DDPF_ALPHAPIXELS : 0u));
    edds_put_u32le(header + 88, 32);
    edds_put_u32le(header + 92, 0x00ff0000u);
    edds_put_u32le(header + 96, 0x0000ff00u);
    edds_put_u32le(header + 100, 0x000000ffu);
    edds_put_u32le(header + 104, source->has_alpha ? 0xff000000u : 0u);
    edds_put_u32le(header + 108, DDSCAPS_TEXTURE |
        (count > 1 ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0u));
}

static edds_status write_edds(
    FILE *output,
    const edds_decoded_source *source,
    const generated_mip *mips,
    uint32_t count,
    edds_error *error
) {
    uint8_t header[DDS_HEADER_BYTES];
    uint8_t descriptor[8];
    dds_header(header, source, count);
    if (fwrite(header, 1, sizeof header, output) != sizeof header) {
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
        profile->generate_mips = 1;
    }
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
    uint32_t count = 0;
    edds_status status;
    if (source == NULL || output == NULL || profile == NULL) {
        edds_fail(error, "invalid-api-argument", "The source, output, and profile are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (profile->format_compress < EDDS_COMPRESS_COPY ||
        profile->format_compress > EDDS_COMPRESS_BEST || profile->compress_threshold > 100u ||
        (profile->generate_mips != 0 && profile->generate_mips != 1)) {
        edds_fail(error, "unsupported-setting", "The conversion profile is outside the supported Workbench slice.");
        return EDDS_UNSUPPORTED_FORMAT;
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
    report(progress, progress_context, 0.15);
    status = generate_mips(&image, profile, mips, &count, cancelled, cancel_context, error);
    if (status == EDDS_OK) {
        report(progress, progress_context, 0.45);
        status = prepare_storage(mips, count, profile, progress, progress_context, error);
    }
    if (status == EDDS_OK) {
        report(progress, progress_context, 0.90);
        status = write_edds(output, &image, mips, count, error);
    }
    free_mips(mips, count);
    free(image.rgba);
    return status;
}
