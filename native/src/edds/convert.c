/*
 * The conversion pipeline: a decoded source image becomes a chain of mips, the profile's swizzle
 * is applied to them, each mip is encoded into the runtime pixel format and stored as it is or
 * LZ4-compressed, and the whole chain is written out as one EDDS file.
 */
#include "memory.h"
#include "image.h"
#include "gpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** The numbers the DDS header is written with. */
enum {
    /** The DDS header, and the DX10 header that follows it when a DXGI format is named. */
    DDS_HEADER_BYTES      = 128,
    DDS_DX10_HEADER_BYTES = 20,

    /** The resource dimension the DX10 header gives: a 2D texture. */
    DDS_RESOURCE_DIMENSION_TEXTURE2D = 3,

    /** The header's flags, at byte 8. */
    DDSD_CAPS        = 0x00000001,
    DDSD_HEIGHT      = 0x00000002,
    DDSD_WIDTH       = 0x00000004,
    DDSD_PITCH       = 0x00000008,
    DDSD_PIXELFORMAT = 0x00001000,
    DDSD_MIPMAPCOUNT = 0x00020000,
    DDSD_LINEARSIZE  = 0x00080000,

    /** The pixel format's flags, at byte 80: RGB (with alpha or not) or a four-character code. */
    DDPF_ALPHAPIXELS = 0x00000001,
    DDPF_FOURCC      = 0x00000004,
    DDPF_RGB         = 0x00000040,

    /** The caps, at byte 108: a texture, and with more than one mip a complex one with mips. */
    DDSCAPS_COMPLEX = 0x00000008,
    DDSCAPS_TEXTURE = 0x00001000,
    DDSCAPS_MIPMAP  = 0x00400000,

    /** The DXGI formats a DX10 header names (see dxgi_format_of). */
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R8_UNORM   = 61,
    DXGI_FORMAT_BC4_UNORM  = 80,
    DXGI_FORMAT_BC5_UNORM  = 83,
    DXGI_FORMAT_BC7_UNORM  = 98
};

/**
 * One level of the mip chain as it goes through the pipeline: its BGRA samples, then the same mip
 * in the runtime format, then the bytes the file stores for it.
 */
typedef struct generated_mip {
    /** The size, and the BGRA samples: `bytes` of them, four per pixel. */
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint8_t *bgra;

    /** Unquantized filtering state; released as soon as the next level is built. */
    float *filter_pixels;

    /** The same mip in the runtime format, which is what the container then compresses. */
    uint32_t payload_bytes;
    uint8_t *payload;

    /** What the file stores: the payload itself (COPY) or its LZ4 frame, and how many bytes. */
    edds_container container;
    uint32_t       stored_bytes;
    uint8_t       *stored;
} generated_mip;

/** How many mips the chain has, each side halving down to 1x1; just 1 without `generate`. */
static uint32_t mip_count(uint32_t width, uint32_t height, int generate) {
    uint32_t count = 1;

    if (!generate) {
        return count;
    }

    while (width > 1 || height > 1) {
        width  = width > 1 ? width / 2u : 1u;
        height = height > 1 ? height / 2u : 1u;
        ++count;
    }

    return count;
}

/** The bytes of a BGRA mip of that size, in `*bytes`; 0 when it is empty or over the limit. */
static int mip_bytes(uint32_t width, uint32_t height, uint32_t *bytes) {
    const uint64_t value = (uint64_t)width * height * 4u;

    if (value == 0 || value > EDDS_MAX_PREVIEW_BYTES) {
        return 0;
    }

    *bytes = (uint32_t)value;

    return 1;
}

/** Copies RGBA samples into the mip as BGRA; without alpha, every pixel is opaque (255). */
static void rgba_mip(const uint8_t *rgba, int has_alpha, generated_mip *mip) {
    for (size_t at = 0; at < mip->bytes; at += 4u) {
        mip->bgra[at]      = rgba[at + 2u];
        mip->bgra[at + 1u] = rgba[at + 1u];
        mip->bgra[at + 2u] = rgba[at];
        mip->bgra[at + 3u] = has_alpha ? rgba[at + 3u] : 255u;
    }
}

/**
 * NVTT keeps the chain in normalized floats and packs through UNORM16 to BGRA8.
 * This turns the mip's float samples into its BGRA bytes that way.
 */
static void pack_filtered_mip(generated_mip *mip) {
    for (size_t at = 0; at < mip->bytes; ++at) {
        /* To 16 bits, held to 0..65535 and rounded; the byte is the high half. */
        float value = mip->filter_pixels[at] * 65535.0f;

        if (value < 0.0f) {
            value = 0.0f;
        }

        if (value > 65535.0f) {
            value = 65535.0f;
        }

        mip->bgra[at] = (uint8_t)((uint32_t)floorf(value + 0.5f) >> 8);
    }
}

/**
 * Builds the next mip of a box-filtered chain from the one above, in floats, one channel at a
 * time. Each new pixel is a weighted mean of the source pixels it covers: two along a side whose
 * source length is even, three along a side whose length is odd.
 */
static void box_float_mip(const generated_mip *previous, generated_mip *next) {
    const uint32_t w = next->width, h = next->height;
    const uint32_t sw = previous->width, sh = previous->height;

    for (uint32_t c = 0; c < 4; ++c) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                /* The source pixel at twice this one's position, and the channel within it. */
                const float *src = previous->filter_pixels + ((size_t)y * 2 * sw + x * 2) * 4 + c;
                float        value;

                if (sw == 1 || sh == 1) {
                    /*
                     * A source one pixel wide or high is a single line: new pixel `at` reads the
                     * line from sample 2 * at, the mean of two samples, or of three weighted
                     * (n - at, n, 1 + at) when the line's length is odd.
                     */
                    const uint32_t n = w * h, at = y * w + x;

                    src   = previous->filter_pixels + (size_t)at * 8 + c;
                    value = ((sw * sh) & 1) != 0
                        ? (1.0f / (float)(2 * n + 1)) *
                            ((float)(n - at) * src[0] +
                                (float)n * src[4] +
                                (float)(1 + at) * src[8])
                        : 0.5f * (src[0] + src[4]);
                } else if ((sw & 1) == 0 && (sh & 1) == 0) {
                    /* Both sides even: the mean of a 2x2 block. */
                    value = 0.25f * (src[0] + src[4] + src[sw * 4] + src[sw * 4 + 4]);
                } else if ((sw & 1) != 0 && (sh & 1) != 0) {
                    /* Both sides odd: a 3x3 block, its weights shifting with the position. */
                    const float wx[3] = { (float)(w - x), (float)w, (float)(1 + x) };
                    const float wy[3] = { (float)(h - y), (float)h, (float)(1 + y) };

                    value = 0;

                    for (uint32_t dy = 0; dy < 3; ++dy) {
                        const float *row = src + dy * sw * 4;

                        value += wy[dy] * (wx[0] * row[0] + wx[1] * row[4] + wx[2] * row[8]);
                    }

                    value *= 1.0f / (float)(sw * sh);
                } else if ((sw & 1) != 0) {
                    /* Only the width odd: three weighted columns of two rows. */
                    value  = (float)(w - x) * (src[0] + src[sw * 4]);
                    value += (float)w * (src[4] + src[sw * 4 + 4]);
                    value += (float)(1 + x) * (src[8] + src[sw * 4 + 8]);
                    value *= 1.0f / (float)(2 * sw);
                } else {
                    /* Only the height odd: three weighted rows of two columns. */
                    value  = (float)(h - y) * (src[0] + src[4]);
                    value += (float)h * (src[sw * 4] + src[sw * 4 + 4]);
                    value += (float)(1 + y) * (src[sw * 8] + src[sw * 8 + 4]);
                    value *= 1.0f / (float)(2 * sh);
                }

                next->filter_pixels[((size_t)y * w + x) * 4 + c] = value;
            }
        }
    }
}

/**
 * I0, the modified Bessel function of the first kind and order zero, summed as its series: terms
 * ((value / 2)^k / k!)^2 are added until one falls below a millionth of the sum.
 */
static float bessel_zero(float value) {
    const float half  = 0.5f * value;
    float       sum   = 1.0f;
    float       power = 1.0f;
    float       delta = 1.0f;
    int         k     = 0;

    while (delta > sum * 1e-6f) {
        ++k;
        power *= half / (float)k;
        delta  = power * power;
        sum   += delta;
    }

    return sum;
}

/** sin(value) / value; within 1e-6 of zero, the first terms of its series instead. */
static float sinc_value(float value) {
    if (fabsf(value) < 1e-6f) {
        return 1.0f + value * value * (-1.0f / 6.0f + value * value / 120.0f);
    }

    return sinf(value) / value;
}

/**
 * The Kaiser filter at `position`: sinc(4 pi position) under a Kaiser window,
 * I0(sqrt(1 - (position / 3)^2)) / I0(1). Further than 3 from the centre it is 0.
 */
static float kaiser_value(float position) {
    const float pi     = 3.14159265358979323846f;
    const float ratio  = position / 3.0f;
    const float inside = 1.0f - ratio * ratio;

    return inside < 0.0f ? 0.0f : sinc_value(pi * position * 4.0f) * bessel_zero(sqrtf(inside)) / bessel_zero(1.0f);
}

/**
 * Which sample a filter tap at `index` reads in a row or column of `length`: past either end, the
 * end sample itself, or for a tiled texture the sample it wraps around to.
 */
static uint32_t sample_index(int index, uint32_t length, int tiled) {
    if (!tiled) {
        if (index < 0) {
            return 0;
        }

        if ((uint32_t)index >= length) {
            return length - 1u;
        }

        return (uint32_t)index;
    }

    int result = index % (int)length;

    return (uint32_t)(result < 0 ? result + (int)length : result);
}

/**
 * The weights of the Kaiser filter's taps for one destination sample: each tap's filter value is
 * averaged over 32 points across it, then all the weights are scaled to add up to 1. Puts the
 * first tap's source index in `*left`, and returns how many taps there are.
 */
static int kaiser_weights(uint32_t source_length, uint32_t destination_length, uint32_t destination, int *left, float weights[20]) {
    /*
     * In source samples: the filter reaches `width` either side of the destination sample's
     * centre, and its taps start at `first`.
     */
    const float scale         = (float)destination_length / (float)source_length;
    const float inverse_scale = 1.0f / scale;
    const float width         = 3.0f * inverse_scale;
    const int   window        = (int)ceilf(width * 2.0f) + 1;
    const float center        = (0.5f + (float)destination) * inverse_scale;
    const int   first         = (int)floorf(center - width);
    float       total         = 0.0f;

    for (int sample = 0; sample < window; ++sample) {
        double integrated = 0.0;

        for (int sub = 0; sub < 32; ++sub) {
            const float position = ((float)(first + sample) - center + ((float)sub + 0.5f) / 32.0f) * scale;

            integrated += kaiser_value(position);
        }

        weights[sample]  = (float)(integrated / 32.0);
        total           += weights[sample];
    }

    for (int sample = 0; sample < window; ++sample) {
        weights[sample] /= total;
    }

    *left = first;

    return window;
}

/**
 * One destination sample of a Kaiser-filtered row or column. `source` holds `source_length`
 * samples, `stride` floats apart; the result is their sum under the weights of kaiser_weights.
 */
static float kaiser_sample_float(
    const float *source,
    uint32_t     source_length,
    uint32_t     destination_length,
    uint32_t     destination,
    size_t       stride,
    int          tiled) {
    float     weights[20];
    int       left;
    const int window = kaiser_weights(source_length, destination_length, destination, &left, weights);
    float     result = 0.0f;

    for (int sample = 0; sample < window; ++sample) {
        result += weights[sample] * source[(size_t)sample_index(left + sample, source_length, tiled) * stride];
    }

    return result;
}

/**
 * Builds the next mip of a Kaiser-filtered chain from the one above, in floats. Each channel is
 * filtered along the rows into a buffer as wide as the next mip, then along the columns into the
 * next mip; a side that keeps its length is copied. Returns 0 when the buffer is not allocated.
 */
static int kaiser_mip(const generated_mip *previous, generated_mip *next, int tiled) {
    const size_t intermediate_count = (size_t)next->width * previous->height;
    float       *intermediate       = edds_alloc(intermediate_count * sizeof *intermediate);

    if (intermediate == NULL) {
        return 0;
    }

    for (uint32_t channel = 0; channel < 4u; ++channel) {
        /* Along the rows: the previous mip's height, the next mip's width. */
        for (uint32_t y = 0; y < previous->height; ++y) {
            for (uint32_t x = 0; x < next->width; ++x) {
                float value;

                if (previous->width == next->width) {
                    value = previous->filter_pixels[((size_t)y * previous->width + x) * 4u + channel];
                } else {
                    value = kaiser_sample_float(
                        previous->filter_pixels + (size_t)y * previous->width * 4u + channel,
                        previous->width, next->width, x, 4u, tiled);
                }

                intermediate[(size_t)y * next->width + x] = value;
            }
        }

        /* Then along the columns, into the next mip. */
        for (uint32_t y = 0; y < next->height; ++y) {
            for (uint32_t x = 0; x < next->width; ++x) {
                float value = previous->height == next->height
                    ? intermediate[(size_t)y * next->width + x]
                    : kaiser_sample_float(intermediate + x, previous->height, next->height, y,
                          next->width, tiled);

                next->filter_pixels[((size_t)y * next->width + x) * 4u + channel] = value;
            }
        }
    }

    edds_free(intermediate);

    return 1;
}

/**
 * Normalizes the mip as a normal map: red, green and blue, read as -1 to 1, are scaled to a
 * vector of length 1 and written back as bytes. Alpha is left as it is.
 */
static void normalize_mip(generated_mip *mip) {
    for (size_t at = 0; at < mip->bytes; at += 4u) {
        float       red    = mip->bgra[at + 2u] * (2.0f / 255.0f) - 1.0f;
        float       green  = mip->bgra[at + 1u] * (2.0f / 255.0f) - 1.0f;
        float       blue   = mip->bgra[at] * (2.0f / 255.0f) - 1.0f;
        const float length = sqrtf(red * red + green * green + blue * blue);

        if (length > 1e-8f) {
            red   /= length;
            green /= length;
            blue  /= length;
        }

        /* DayZ's source normalizer truncates (unit + 1) * 128.5, saturated to a byte. */
        mip->bgra[at + 2u] = (uint8_t)fminf(255.0f, fmaxf(0.0f, (red + 1.0f) * 128.5f));
        mip->bgra[at + 1u] = (uint8_t)fminf(255.0f, fmaxf(0.0f, (green + 1.0f) * 128.5f));
        mip->bgra[at]      = (uint8_t)fminf(255.0f, fmaxf(0.0f, (blue + 1.0f) * 128.5f));
    }
}

/** The same normalization on the mip's float samples, which stay floats from 0 to 1. */
static void normalize_filtered_mip(generated_mip *mip) {
    for (size_t at = 0; at < mip->bytes; at += 4u) {
        float      *p = mip->filter_pixels + at;
        float       r = p[2] * 2.0f - 1.0f, g = p[1] * 2.0f - 1.0f, b = p[0] * 2.0f - 1.0f;
        const float length = sqrtf(r * r + g * g + b * b);

        if (length > 0.0f) {
            r /= length;
            g /= length;
            b /= length;
        }

        p[2] = r * 0.5f + 0.5f;
        p[1] = g * 0.5f + 0.5f;
        p[0] = b * 0.5f + 0.5f;
    }
}

/** A 16-bit hash of the four bytes at `at`, read as a little-endian number. */
static uint32_t lz4_hash(const uint8_t *at) {
    const uint32_t value = (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);

    return (value * 2654435761u) >> 16;
}

/**
 * Writes the part of a length that did not fit its 4 bits in the token: a byte of 255 for every
 * 255 of it, then a byte with the rest. Returns 0 when `output` runs out.
 */
static int write_lz4_length(uint8_t *output, size_t capacity, size_t *at, size_t length) {
    while (length >= 255u) {
        if (*at >= capacity) {
            return 0;
        }

        output[(*at)++]  = 255;
        length          -= 255u;
    }

    if (*at >= capacity) {
        return 0;
    }

    output[(*at)++] = (uint8_t)length;

    return 1;
}

/**
 * Compresses one block of `size` bytes into LZ4 sequences in `output`. Each sequence is a run of
 * bytes copied as they are (literals) and then a match: a copy of earlier bytes, found through a
 * hash of the next four bytes and the chain of earlier positions with the same hash. Returns 1
 * with the compressed size in `*written`; 0 when it does not fit `capacity` or memory runs out.
 */
static int lz4_block(
    const uint8_t *input,
    size_t         size,
    unsigned       search_depth,
    uint8_t       *output,
    size_t         capacity,
    size_t        *written) {
    /* Per hash, its latest position; per position, the one before it with the same hash. */
    int32_t *head     = edds_alloc(65536u * sizeof *head);
    int32_t *previous = edds_alloc((size == 0 ? 1u : size) * sizeof *previous);

    /* Where reading is, where the unwritten literals start, where writing is, and the result. */
    size_t input_at  = 0;
    size_t anchor    = 0;
    size_t output_at = 0;
    int    ok        = 0;

    if (head == NULL || previous == NULL) {
        goto done;
    }

    /* No hash has a position yet (-1). */
    memset(head, 0xff, 65536u * sizeof *head);

    /* Matches are looked for while 12 bytes or more are left; the rest are literals below. */
    while (input_at + 12u <= size) {
        const uint32_t hash        = lz4_hash(input + input_at);
        int32_t        candidate   = head[hash];
        size_t         best_length = 0;
        size_t         best_offset = 0;
        unsigned       searched    = 0;

        /* This position goes to the front of its hash's chain. */
        previous[input_at] = candidate;
        head[hash]         = (int32_t)input_at;

        /*
         * The longest match among earlier positions on the chain: at most `search_depth` of them,
         * at most 65535 bytes back, and never into the last 5 bytes of the block.
         */
        while (candidate >= 0 && searched++ < search_depth && input_at - (size_t)candidate <= 65535u) {
            size_t       length  = 0;
            const size_t maximum = size - input_at - 5u;

            while (length < maximum && input[(size_t)candidate + length] == input[input_at + length]) {
                ++length;
            }

            if (length > best_length && length >= 4u) {
                best_length = length;
                best_offset = input_at - (size_t)candidate;

                if (search_depth == 1u || length == maximum) {
                    break;
                }
            }

            candidate = previous[candidate];
        }

        /* No match of 4 bytes or more: this byte stays a literal. */
        if (best_length < 4u) {
            ++input_at;
            continue;
        }

        /*
         * One sequence: a token byte (the literal count in its high 4 bits, the match length less
         * 4 in its low 4, and 15 meaning more length bytes follow), the literals, the match's
         * 2-byte offset back, and the rest of the match length.
         */
        {
            const size_t literals   = input_at - anchor;
            const size_t match_code = best_length - 4u;
            const size_t token_at   = output_at++;

            if (token_at >= capacity) {
                goto done;
            }

            output[token_at] = (uint8_t)((literals < 15u ? literals : 15u) << 4);

            if (literals >= 15u && !write_lz4_length(output, capacity, &output_at, literals - 15u)) {
                goto done;
            }

            if (literals > capacity - output_at) {
                goto done;
            }

            memcpy(output + output_at, input + anchor, literals);
            output_at += literals;

            if (capacity - output_at < 2u) {
                goto done;
            }

            output[output_at++]  = (uint8_t)best_offset;
            output[output_at++]  = (uint8_t)(best_offset >> 8);
            output[token_at]    |= (uint8_t)(match_code < 15u ? match_code : 15u);

            if (match_code >= 15u && !write_lz4_length(output, capacity, &output_at, match_code - 15u)) {
                goto done;
            }
        }

        /* The positions inside the match join their chains too; reading goes on after it. */
        for (size_t index = 1; index < best_length && input_at + index + 4u <= size; ++index) {
            const size_t   position      = input_at + index;
            const uint32_t inserted_hash = lz4_hash(input + position);

            previous[position]  = head[inserted_hash];
            head[inserted_hash] = (int32_t)position;
        }

        input_at += best_length;
        anchor    = input_at;
    }

    /* The last sequence: the literals that are left, and no match after them. */
    {
        const size_t literals = size - anchor;
        const size_t token_at = output_at++;

        if (token_at >= capacity) {
            goto done;
        }

        output[token_at] = (uint8_t)((literals < 15u ? literals : 15u) << 4);

        if (literals >= 15u && !write_lz4_length(output, capacity, &output_at, literals - 15u)) {
            goto done;
        }

        if (literals > capacity - output_at) {
            goto done;
        }

        memcpy(output + output_at, input + anchor, literals);
        output_at += literals;
    }

    *written = output_at;
    ok       = 1;

done:
    edds_free(head);
    edds_free(previous);
    return ok;
}

/**
 * LZ4-compresses one mip into the frame the container stores: the mip's size in 4 bytes, then
 * each block of up to 64 KiB as a 4-byte descriptor (its compressed size, with the top bit set on
 * the last block) and its compressed bytes. Returns the frame, which the caller frees, and its
 * size in `*stored_bytes`; NULL when memory runs out or the frame outgrows its room or 32 bits.
 */
static uint8_t *lz4_frame(
    const uint8_t       *input,
    uint32_t             size,
    edds_format_compress mode,
    uint32_t            *stored_bytes) {
    /* How many blocks, and one allocation for the whole frame, sized before anything is packed. */
    const uint32_t block_count = (size + 65535u) / 65536u;
    const size_t   capacity    = 4u + (size_t)block_count * (4u + 16u) + size + size / 255u;

    /* How many earlier positions a match search may try: Fastest 1, Medium 16, Best 64. */
    const unsigned depth = mode == EDDS_COMPRESS_FASTEST ? 1u : (mode == EDDS_COMPRESS_MEDIUM ? 16u : 64u);

    uint8_t *frame     = edds_alloc(capacity);
    size_t   output_at = 4;
    uint32_t input_at  = 0;

    if (frame == NULL) {
        return NULL;
    }

    edds_put_u32le(frame, size);

    for (uint32_t block = 0; block < block_count; ++block) {
        const uint32_t block_bytes     = size - input_at > 65536u ? 65536u : size - input_at;
        const size_t   descriptor_at   = output_at;
        size_t         compressed_size = 0;

        /* The descriptor's 4 bytes are left for when the block's compressed size is known. */
        output_at += 4;

        if (!lz4_block(input + input_at, block_bytes, depth, frame + output_at,
                capacity - output_at, &compressed_size) ||
            compressed_size > UINT32_MAX) {
            edds_free(frame);
            return NULL;
        }

        edds_put_u32le(frame + descriptor_at, (block + 1u == block_count ? 0x80000000u : 0u) | (uint32_t)compressed_size);
        output_at += compressed_size;
        input_at  += block_bytes;
    }

    if (output_at > UINT32_MAX) {
        edds_free(frame);
        return NULL;
    }

    *stored_bytes = (uint32_t)output_at;

    return frame;
}

/** Reports progress, when the caller asked for it. */
static void report(edds_progress_fn progress, void *context, double value) {
    if (progress != NULL) {
        progress(context, value);
    }
}

/**
 * Decides what the file stores for each mip: the payload as it is, or its LZ4 frame when the
 * profile compresses and the frame is at most `compress_threshold` percent of the payload.
 * Progress moves from 0.45 towards 0.9 as it goes.
 */
static edds_status prepare_storage(
    generated_mip      *mips,
    uint32_t            count,
    const edds_profile *profile,
    edds_progress_fn    progress,
    void               *progress_context,
    edds_error         *error) {
    for (uint32_t at = 0; at < count; ++at) {
        generated_mip *mip = &mips[at];

        /* Container compression is the long part of a conversion, so it moves the row per mip. */
        report(progress, progress_context, 0.45 + 0.45 * ((double)at / (double)count));

        /* Stored as it is, unless the frame below wins. */
        mip->container    = EDDS_CONTAINER_COPY;
        mip->stored_bytes = mip->payload_bytes;
        mip->stored       = mip->payload;

        if (profile->format_compress != EDDS_COMPRESS_COPY) {
            uint32_t compressed_bytes = 0;

            uint8_t *compressed = lz4_frame(mip->payload, mip->payload_bytes, profile->format_compress, &compressed_bytes);

            if (compressed == NULL) {
                edds_fail(error, "allocation-failed", "Memory for LZ4 container compression could not be allocated.");
                return EDDS_INTERNAL_FAILURE;
            }

            if ((uint64_t)compressed_bytes * 100u <= (uint64_t)mip->payload_bytes * profile->compress_threshold) {
                mip->container    = EDDS_CONTAINER_LZ4;
                mip->stored_bytes = compressed_bytes;
                mip->stored       = compressed;
            } else {
                edds_free(compressed);
            }
        }
    }

    return EDDS_OK;
}

/**
 * Frees every buffer of the first `count` mips and clears their pointers. The stored bytes are
 * freed on their own only when they are not the payload itself.
 */
static void free_mips(generated_mip *mips, uint32_t count) {
    for (uint32_t at = 0; at < count; ++at) {
        if (mips[at].stored != mips[at].payload) {
            edds_free(mips[at].stored);
        }

        edds_free(mips[at].payload);
        edds_free(mips[at].bgra);
        edds_free(mips[at].filter_pixels);

        mips[at].filter_pixels = NULL;
        mips[at].bgra          = NULL;
        mips[at].payload       = NULL;
        mips[at].stored        = NULL;
    }
}

/**
 * Turns the staged BGRA chain into the runtime format. Container compression happens afterwards
 * and over these bytes, so `FormatCompress` cannot change a single decoded pixel of the result.
 * Returns EDDS_OK, or the refusal when a mip is too large, memory runs out or the run is cancelled.
 */
static edds_status encode_mips(
    generated_mip    *mips,
    uint32_t          count,
    edds_pixel_format format,
    uint32_t          quality,
    edds_cancelled_fn cancelled,
    void             *context,
    edds_error       *error) {
    for (uint32_t at = 0; at < count; ++at) {
        generated_mip *mip = &mips[at];

        mip->payload_bytes = edds_gpu_mip_bytes(format, mip->width, mip->height);

        if (mip->payload_bytes == 0) {
            edds_fail(error, "mip-size-limit", "A generated mip exceeds the runtime-format limit.");
            return EDDS_INVALID_INPUT;
        }

        mip->payload = edds_alloc(mip->payload_bytes);

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

/**
 * Builds the BGRA mip chain from the source: the supplied mips copied, or the image followed by
 * each level filtered down from the one above; then RemoveMips drops the largest levels. Returns
 * EDDS_OK with the levels in `mips` and their number in `*count`; on a refusal, nothing is left
 * allocated.
 */
static edds_status generate_chain(
    const edds_decoded_source *source,
    const edds_profile        *profile,
    generated_mip             *mips,
    uint32_t                  *count,
    edds_cancelled_fn          cancelled,
    void                      *context,
    edds_error                *error) {
    if (profile->contains_mips && source->supplied_mip_count != mip_count(source->width, source->height, 1)) {
        edds_fail(error, "unsupported-dds-mip-layout", "ContainsMips=true requires one complete largest-to-smallest DDS mip chain.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    /* The full chain: the supplied mips, or every level down to 1x1 (one without GenerateMips). */
    const uint32_t complete_count = profile->contains_mips
        ? source->supplied_mip_count
        : mip_count(source->width, source->height, profile->generate_mips);

    if (complete_count == 0 || profile->remove_mips >= complete_count) {
        edds_fail(error, "remove-mips-out-of-range",
            "RemoveMips=%u would remove a missing level or the complete mip chain.",
            profile->remove_mips);
        return EDDS_INVALID_INPUT;
    }

    /* Generated levels are filtered in floats: each level keeps a float copy for the next one. */
    const int float_filter = profile->generate_mips;

    *count = complete_count;
    memset(mips, 0, sizeof(*mips) * complete_count);
    mips[0].width  = source->width;
    mips[0].height = source->height;

    /* Level by level, the largest first. */
    for (uint32_t at = 0; at < complete_count; ++at) {
        if (cancelled != NULL && cancelled(context)) {
            free_mips(mips, complete_count);
            edds_fail(error, "cancelled", "The conversion was cancelled.");
            return EDDS_CANCELLED;
        }

        /* The level's size: the supplied mip's, or half the level above, at least 1. */
        if (profile->contains_mips) {
            mips[at].width  = source->supplied_mips[at].width;
            mips[at].height = source->supplied_mips[at].height;
        } else if (at > 0) {
            mips[at].width  = mips[at - 1u].width > 1 ? mips[at - 1u].width / 2u : 1u;
            mips[at].height = mips[at - 1u].height > 1 ? mips[at - 1u].height / 2u : 1u;
        }

        if (!mip_bytes(mips[at].width, mips[at].height, &mips[at].bytes)) {
            free_mips(mips, complete_count);
            edds_fail(error, "mip-size-limit", "A generated mip exceeds the decoded-image limit.");
            return EDDS_INVALID_INPUT;
        }

        mips[at].bgra = edds_alloc(mips[at].bytes);

        if (mips[at].bgra == NULL) {
            free_mips(mips, complete_count);
            edds_fail(error, "allocation-failed", "Memory for the mip chain could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }

        if (float_filter) {
            mips[at].filter_pixels = edds_alloc((size_t)mips[at].bytes * sizeof(float));

            if (mips[at].filter_pixels == NULL) {
                free_mips(mips, complete_count);
                edds_fail(error, "allocation-failed", "Memory for mip filtering could not be allocated.");
                return EDDS_INTERNAL_FAILURE;
            }
        }

        /*
         * The level's samples: the supplied mip's, the image's for the top level, or filtered
         * down from the level above with the Kaiser filter, or else with the box filter.
         */
        if (profile->contains_mips) {
            rgba_mip(source->supplied_mips[at].rgba, source->has_alpha, &mips[at]);
        } else if (at == 0) {
            rgba_mip(source->rgba, source->has_alpha, &mips[at]);
        } else {
            if (profile->mipmap_function != EDDS_MIPMAP_NORMALIZE && profile->mipmap_filter == EDDS_FILTER_KAISER) {
                if (!kaiser_mip(&mips[at - 1u], &mips[at], profile->tiled_texture)) {
                    free_mips(mips, complete_count);
                    edds_fail(error, "allocation-failed", "Memory for Kaiser mip filtering could not be allocated.");
                    return EDDS_INTERNAL_FAILURE;
                }
            } else {
                box_float_mip(&mips[at - 1u], &mips[at]);
            }

            if (profile->mipmap_function == EDDS_MIPMAP_NORMALIZE) {
                normalize_filtered_mip(&mips[at]);
            }

            if (float_filter) {
                pack_filtered_mip(&mips[at]);
            }
        }

        /* Normalize applies to levels taken from the source, not to filtered ones. */
        if (profile->normalize && (profile->contains_mips || at == 0)) {
            normalize_mip(&mips[at]);
        }

        /*
         * The top level's float copy is made from its bytes; once a lower level is built, the
         * float copy of the level above it is not needed any more.
         */
        if (float_filter) {
            if (at == 0) {
                for (size_t pixel = 0; pixel < mips[at].bytes; ++pixel) {
                    mips[at].filter_pixels[pixel] = mips[at].bgra[pixel] * (1.0f / 255.0f);
                }
            } else {
                edds_free(mips[at - 1].filter_pixels);
                mips[at - 1].filter_pixels = NULL;
            }
        }
    }

    /* Nothing is built from the last level, so its float copy goes too. */
    edds_free(mips[complete_count - 1].filter_pixels);
    mips[complete_count - 1].filter_pixels = NULL;

    /* RemoveMips: the largest levels are freed, and the rest move up to the front. */
    if (profile->remove_mips != 0) {
        const uint32_t removed = profile->remove_mips;

        for (uint32_t at = 0; at < removed; ++at) {
            edds_free(mips[at].bgra);
            mips[at].bgra = NULL;
        }

        memmove(mips, mips + removed, sizeof(*mips) * (complete_count - removed));
        memset(mips + complete_count - removed, 0, sizeof(*mips) * removed);
        *count = complete_count - removed;
    }

    return EDDS_OK;
}

/**
 * Resampling is in normalized float bytes, with truncation back to a byte.
 * One channel of `source` at the point (`x`, `y`), mixed from the four pixels around it by how
 * near the point is to each.
 */
static uint8_t terrain_sample(const generated_mip *source, float x, float y, uint32_t channel) {
    /* The pixels around the point; past the right or bottom edge, the edge pixel again. */
    const uint32_t left = (uint32_t)x, top = (uint32_t)y;
    const uint32_t right  = left + 1u < source->width ? left + 1u : left;
    const uint32_t bottom = top + 1u < source->height ? top + 1u : top;

    /* How far the point is past the left and the top pixel, from 0 to 1. */
    const float fx = x - (float)left, fy = y - (float)top;

    /* The four samples, from 0 to 1. */
    const float a = source->bgra[((size_t)top * source->width + left) * 4u + channel] * (1.0f / 255.0f);
    const float b = source->bgra[((size_t)top * source->width + right) * 4u + channel] * (1.0f / 255.0f);
    const float c = source->bgra[((size_t)bottom * source->width + left) * 4u + channel] * (1.0f / 255.0f);
    const float d = source->bgra[((size_t)bottom * source->width + right) * 4u + channel] * (1.0f / 255.0f);

    /* Mixed across, then down, and back to the byte range. */
    const float value = ((a * (1.0f - fx) + b * fx) * (1.0f - fy) + (c * (1.0f - fx) + d * fx) * fy) * 255.0f;

    return (uint8_t)fminf(255.0f, fmaxf(0.0f, value));
}

/**
 * Whether the profile resamples for a terrain swizzle: TerrainLayerTexture or TerrainSuperTexture
 * swizzling, with the None or the DXT conversion.
 */
static int uses_terrain_resampling(const edds_profile *profile) {
    return (profile->conversion == EDDS_CONVERSION_NONE ||
               profile->conversion == EDDS_CONVERSION_DXT) &&
        (profile->swizzling == EDDS_SWIZZLE_TERRAIN_LAYER ||
            profile->swizzling == EDDS_SWIZZLE_TERRAIN_SUPER);
}

/** Whether the profile reduces for the AmbientSpecularMapGA swizzle: any conversion but Red. */
static int uses_ambient_reduction(const edds_profile *profile) {
    return profile->swizzling == EDDS_SWIZZLE_AMBIENT_SPECULAR && profile->conversion != EDDS_CONVERSION_RED;
}

/**
 * Terrain/ambient removal creates a new top level, then runs the ordinary mip generator.
 * Without RemoveMips, or for any other swizzle, the chain is built from the source as it is.
 */
static edds_status generate_mips(const edds_decoded_source *source, const edds_profile *profile,
    generated_mip *mips, uint32_t *count,
    edds_cancelled_fn cancelled, void *context, edds_error *error) {
    const int terrain = uses_terrain_resampling(profile);
    const int ambient = uses_ambient_reduction(profile);

    if (profile->remove_mips == 0 || (!terrain && !ambient)) {
        return generate_chain(source, profile, mips, count, cancelled, context, error);
    }

    if (profile->contains_mips || source->width < 8 || source->height < 8 || (ambient && !source->has_alpha)) {
        edds_fail(error, "unsupported-combination",
            "Terrain/ambient RemoveMips requires an unsupplied source of at least 8x8; ambient also requires alpha.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (profile->remove_mips >= mip_count(source->width, source->height, profile->generate_mips)) {
        edds_fail(error, "remove-mips-out-of-range", "RemoveMips would remove the complete mip chain.");
        return EDDS_INVALID_INPUT;
    }

    /* The new top level, the source as BGRA, and the profile its chain is built with. */
    edds_decoded_source reduced       = { 0 };
    generated_mip       original      = { 0 };
    edds_profile        chain_profile = *profile;

    /* The new top level is the source halved RemoveMips times, but at least 8 on each side. */
    reduced.width  = source->width >> profile->remove_mips;
    reduced.height = source->height >> profile->remove_mips;

    if (reduced.width < 8) {
        reduced.width = 8;
    }

    if (reduced.height < 8) {
        reduced.height = 8;
    }

    reduced.has_alpha = source->has_alpha;
    original.width    = source->width;
    original.height   = source->height;

    if (!mip_bytes(original.width, original.height, &original.bytes)) {
        return EDDS_INVALID_INPUT;
    }

    original.bgra = edds_alloc(original.bytes);
    reduced.rgba  = edds_alloc((size_t)reduced.width * reduced.height * 4u);

    if (original.bgra == NULL || reduced.rgba == NULL) {
        edds_free(original.bgra);
        edds_free(reduced.rgba);
        edds_fail(error, "allocation-failed", "Memory for swizzle resampling could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }

    /* The source as BGRA, normalized first when the profile asks for it. */
    rgba_mip(source->rgba, source->has_alpha, &original);

    if (profile->normalize) {
        normalize_mip(&original);
    }

    /* The new top level, pixel by pixel. */
    for (uint32_t y = 0; y < reduced.height; ++y) {
        if (cancelled != NULL && cancelled(context)) {
            edds_free(original.bgra);
            edds_free(reduced.rgba);
            edds_fail(error, "cancelled", "The conversion was cancelled.");
            return EDDS_CANCELLED;
        }

        for (uint32_t x = 0; x < reduced.width; ++x) {
            uint8_t bgra[4];

            if (ambient) {
                /*
                 * Ambient: over the block of the source this pixel covers, the lowest value of
                 * each colour channel and the highest alpha.
                 */
                const uint32_t sx = source->width / reduced.width,
                               sy = source->height / reduced.height;

                bgra[0] = bgra[1] = bgra[2] = 255;
                bgra[3]                     = 0;

                for (uint32_t dy = 0; dy < sy; ++dy) {
                    for (uint32_t dx = 0; dx < sx; ++dx) {
                        const uint8_t *p = original.bgra + ((size_t)(y * sy + dy) * original.width + x * sx + dx) * 4u;

                        for (uint32_t c = 0; c < 3; ++c) {
                            if (p[c] < bgra[c]) {
                                bgra[c] = p[c];
                            }
                        }

                        if (p[3] > bgra[3]) {
                            bgra[3] = p[3];
                        }
                    }
                }
            } else {
                /* Workbench duplicates three border texels after resampling the interior. */
                const uint32_t ix = x < 3 ? 3 : x >= reduced.width - 3 ? reduced.width - 4
                                                                       : x;
                const uint32_t iy = y < 3 ? 3 : y >= reduced.height - 3 ? reduced.height - 4
                                                                        : y;
                float          px, py;

                if (profile->swizzling == EDDS_SWIZZLE_TERRAIN_LAYER) {
                    /* TerrainLayer: the first and last pixels of both images line up. */
                    px = (float)ix * ((float)(source->width - 1) / (float)(reduced.width - 1));
                    py = (float)iy * ((float)(source->height - 1) / (float)(reduced.height - 1));
                } else {
                    /*
                     * TerrainSuper: the interior, 3 pixels in from each edge, is stretched over
                     * the source's interior. Its last row and column land exactly on the source's
                     * last interior ones, and the pixels on its border on whole source pixels.
                     */
                    px = (float)(ix - 3) * (1.0f / (float)(reduced.width - 6)) * (float)(source->width - 6) + 3.0f;
                    py = (float)(iy - 3) * (1.0f / (float)(reduced.height - 6)) * (float)(source->height - 6) + 3.0f;

                    if (ix == reduced.width - 4) {
                        px = (float)(source->width - 4);
                    }

                    if (iy == reduced.height - 4) {
                        py = (float)(source->height - 4);
                    }

                    if (ix == 3 || ix == reduced.width - 4 || iy == 3 || iy == reduced.height - 4) {
                        px = nearbyintf(px);
                        py = nearbyintf(py);
                    }
                }

                for (uint32_t c = 0; c < 4; ++c) {
                    bgra[c] = terrain_sample(&original, px, py, c);
                }
            }

            /* Back to RGBA, into the new top level. */
            uint8_t *p = reduced.rgba + ((size_t)y * reduced.width + x) * 4u;

            p[0] = bgra[2];
            p[1] = bgra[1];
            p[2] = bgra[0];
            p[3] = bgra[3];
        }
    }

    edds_free(original.bgra);

    /* The chain is built from the new top level: nothing more to remove, normalized already. */
    chain_profile.remove_mips = 0;
    chain_profile.normalize   = 0;

    const edds_status status = generate_chain(&reduced, &chain_profile, mips, count, cancelled, context, error);

    edds_free(reduced.rgba);

    return status;
}

/** Frees a decoded source's samples, its supplied mips or its one image, and clears it. */
static void free_source(edds_decoded_source *source) {
    if (source->supplied_mip_count != 0) {
        for (uint32_t level = 0; level < source->supplied_mip_count; ++level) {
            edds_free(source->supplied_mips[level].rgba);
        }
    } else {
        edds_free(source->rgba);
    }

    memset(source, 0, sizeof *source);
}

/** The DXGI format a DX10 header names, or 0 for a format that has a legacy descriptor. */
static uint32_t dxgi_format_of(edds_pixel_format format) {
    switch (format) {
        case EDDS_PIXEL_R8:  return DXGI_FORMAT_R8_UNORM;
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
 * Returns how many bytes of `header` there are to write: the header, and the DX10 one if any.
 */
static uint32_t dds_header(
    uint8_t           header[DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES],
    edds_pixel_format format,
    uint32_t          width,
    uint32_t          height,
    uint32_t          count,
    uint32_t          top_mip_bytes) {
    const uint32_t block = edds_gpu_block_bytes(format);
    const uint32_t dxgi  = dxgi_format_of(format);
    const uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT |
        DDSD_MIPMAPCOUNT | (block != 0 ? DDSD_LINEARSIZE : DDSD_PITCH);

    /* What every format shares, from a zeroed header: "DDS ", 124 at byte 4, and the flags. */
    memset(header, 0, DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES);
    memcpy(header, "DDS ", 4);
    edds_put_u32le(header + 4, 124);
    edds_put_u32le(header + 8, flags);

    /* The top mip's height and width, its linear size or pitch, and the mip count. */
    edds_put_u32le(header + 12, height);
    edds_put_u32le(header + 16, width);
    edds_put_u32le(header + 20, block != 0 ? top_mip_bytes : width * edds_gpu_pixel_bytes(format));
    edds_put_u32le(header + 28, count);

    /* The "ENF1" marker at byte 36, 32 at byte 76, and the caps. */
    memcpy(header + 36, "ENF1", 4);
    edds_put_u32le(header + 76, 32);
    edds_put_u32le(header + 108, DDSCAPS_TEXTURE | (count > 1 ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0u));

    /* BGRA and BGRX: 32-bit RGB with the mask of each channel, an alpha mask only for BGRA. */
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

    /* Every other format is named by a four-character code: DXT1, DXT5, or DX10. */
    edds_put_u32le(header + 80, DDPF_FOURCC);

    if (dxgi == 0) {
        memcpy(header + 84, format == EDDS_PIXEL_DXT1 ? "DXT1" : "DXT5", 4);
        return DDS_HEADER_BYTES;
    }

    /* The DX10 header: the DXGI format, a 2D texture, no misc flag, an array of one, and 0. */
    memcpy(header + 84, "DX10", 4);
    edds_put_u32le(header + 128, dxgi);
    edds_put_u32le(header + 132, DDS_RESOURCE_DIMENSION_TEXTURE2D);
    edds_put_u32le(header + 136, 0);
    edds_put_u32le(header + 140, 1);
    edds_put_u32le(header + 144, 0);

    return DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES;
}

/**
 * Writes the EDDS file: the DDS header, then one 8-byte descriptor per mip ("COPY" or "LZ4 " and
 * the stored size), then the stored bytes of every mip. Both lists run from the smallest mip to
 * the largest. A write that fails ends it with "output-write-failed".
 */
static edds_status write_edds(
    FILE                *output,
    edds_pixel_format    format,
    const generated_mip *mips,
    uint32_t             count,
    edds_error          *error) {
    uint8_t        header[DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES];
    uint8_t        descriptor[8];
    const uint32_t header_bytes = dds_header(header, format, mips[0].width, mips[0].height, count, mips[0].payload_bytes);

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

/**
 * Fills in the default profile: Fastest compression with a threshold of 80, no conversion and no
 * swizzling, quality 1, generated mips filtered with Box, and a tiled texture.
 */
void edds_default_profile(edds_profile *profile) {
    if (profile != NULL) {
        profile->format_compress    = EDDS_COMPRESS_FASTEST;
        profile->compress_threshold = 80;
        profile->conversion         = EDDS_CONVERSION_NONE;
        profile->swizzling          = EDDS_SWIZZLE_NONE;
        profile->conversion_quality = EDDS_QUALITY_SCALE;
        profile->remove_mips        = 0;
        profile->contains_mips      = 0;
        profile->generate_mips      = 1;
        profile->normalize          = 0;
        profile->mipmap_function    = EDDS_MIPMAP_FILTER;
        profile->mipmap_filter      = EDDS_FILTER_BOX;
        profile->tiled_texture      = 1;
    }
}

/**
 * Checks the profile one rule at a time: known values, each within its range, and only the
 * combinations this converter supports. Returns EDDS_OK, or the first refusal with `error` filled.
 */
edds_status edds_profile_check(const edds_profile *profile, edds_error *error) {
    const edds_conversion_capability *conversion;

    if (profile == NULL) {
        edds_fail(error, "invalid-api-argument", "A conversion profile is required.");
        return EDDS_INTERNAL_FAILURE;
    }

    if (edds_swizzle_capability_of(profile->swizzling) == NULL) {
        edds_fail(error, "unsupported-setting", "Workbench setting Swizzling is unknown or unsupported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (uses_terrain_resampling(profile) && (!profile->generate_mips || profile->contains_mips)) {
        edds_fail(error, "unsupported-combination",
            "Terrain layer/super swizzling requires generated mips; supplied or disabled mips are incompatible.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (uses_ambient_reduction(profile) && profile->remove_mips > 0 && profile->contains_mips) {
        edds_fail(error, "unsupported-combination", "AmbientSpecularMapGA with RemoveMips requires an unsupplied source.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    /* Every number within its range, and every switch 0 or 1. */
    if (profile->format_compress < EDDS_COMPRESS_COPY ||
        profile->format_compress > EDDS_COMPRESS_BEST ||
        profile->compress_threshold > 100u ||
        profile->remove_mips > 14u ||
        (profile->contains_mips != 0 && profile->contains_mips != 1) ||
        (profile->generate_mips != 0 && profile->generate_mips != 1) ||
        (profile->normalize != 0 && profile->normalize != 1) ||
        (profile->tiled_texture != 0 && profile->tiled_texture != 1)) {
        edds_fail(error, "unsupported-setting", "The conversion profile is outside the supported Workbench slice.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (profile->contains_mips && profile->generate_mips) {
        edds_fail(error, "unsupported-combination", "Workbench settings ContainsMips=true and GenerateMips=true are mutually exclusive.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    /* DayZ's ColorNoise mip function follows Filter. Its noise-producing swizzle is separate. */
    if (profile->mipmap_function < EDDS_MIPMAP_FILTER || profile->mipmap_function > EDDS_MIPMAP_COLOR_NOISE) {
        edds_fail(error, "unsupported-setting", "Workbench setting MipMapFunction is unknown.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (profile->mipmap_filter == EDDS_FILTER_TRIANGLE) {
        edds_fail(error, "unsupported-setting", "Workbench setting MipMapFilter=Triangle is recognized but unsupported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (profile->mipmap_filter < EDDS_FILTER_BOX || profile->mipmap_filter > EDDS_FILTER_TRIANGLE) {
        edds_fail(error, "unsupported-setting", "Workbench setting MipMapFilter is unknown.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (!profile->generate_mips && profile->mipmap_function != EDDS_MIPMAP_FILTER) {
        edds_fail(error, "unsupported-combination", "Workbench setting MipMapFunction is active only while GenerateMips=true.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if ((!profile->generate_mips || profile->mipmap_function == EDDS_MIPMAP_NORMALIZE) && profile->mipmap_filter != EDDS_FILTER_BOX) {
        edds_fail(error, "unsupported-combination",
            "Workbench setting MipMapFilter requires GenerateMips=true and MipMapFunction=Filter or ColorNoise.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    /* The conversion must be one this converter implements. */
    conversion = edds_conversion_capability_of(profile->conversion);

    if (conversion == NULL || !conversion->supported) {
        edds_fail(error, "unsupported-setting",
            "Workbench setting Conversion=%s is recognized but unsupported.",
            conversion == NULL ? "unknown" : conversion->workbench_name);
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (profile->conversion_quality > EDDS_QUALITY_SCALE) {
        edds_fail(error, "unsupported-setting", "Workbench setting ConversionQuality must be between 0 and 1.");
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
 * Returns the runtime format, or EDDS_PIXEL_UNKNOWN for a conversion this does not know.
 */
edds_pixel_format edds_profile_pixel_format(const edds_profile *profile, edds_source_alpha alpha) {
    const edds_swizzle_capability *swizzle = profile == NULL ? NULL : edds_swizzle_capability_of(profile->swizzling);

    if (swizzle != NULL && swizzle->writes_alpha) {
        /* These mappings produce BGRA even when the original source was opaque RGB. */
        if (profile->conversion == EDDS_CONVERSION_NONE) {
            return EDDS_PIXEL_BGRA8;
        }

        if (profile->conversion == EDDS_CONVERSION_DXT) {
            return EDDS_PIXEL_DXT5;
        }
    }

    /* Non-default identity mappings retain declared alpha in the captured DXT branch too. */
    if (swizzle != NULL &&
        swizzle->swizzling != EDDS_SWIZZLE_NONE &&
        profile->conversion == EDDS_CONVERSION_DXT &&
        alpha != EDDS_ALPHA_ABSENT) {
        return EDDS_PIXEL_DXT5;
    }

    /* Otherwise the conversion alone decides, None and DXT by the alpha. */
    switch (profile == NULL ? EDDS_CONVERSION_NONE : profile->conversion) {
        case EDDS_CONVERSION_NONE:
            return alpha == EDDS_ALPHA_ABSENT ? EDDS_PIXEL_BGRX8 : EDDS_PIXEL_BGRA8;

        case EDDS_CONVERSION_DXT:
            return alpha == EDDS_ALPHA_USED ? EDDS_PIXEL_DXT5 : EDDS_PIXEL_DXT1;

        case EDDS_CONVERSION_RED:          return EDDS_PIXEL_R8;
        case EDDS_CONVERSION_RED_HQ:       return EDDS_PIXEL_BC4;
        case EDDS_CONVERSION_RED_GREEN:    return EDDS_PIXEL_RG8;
        case EDDS_CONVERSION_RED_GREEN_HQ: return EDDS_PIXEL_BC5;
        case EDDS_CONVERSION_COLOR_HQ:     return EDDS_PIXEL_BC7;

        default: return EDDS_PIXEL_UNKNOWN;
    }
}

/**
 * Read off this one source's own samples, never off a batch and never off a file name.
 * ABSENT when the source declares no alpha; USED when any sample, in any supplied mip, is below
 * 255; OPAQUE otherwise.
 */
static edds_source_alpha source_alpha_of(const edds_decoded_source *source) {
    if (!source->has_alpha) {
        return EDDS_ALPHA_ABSENT;
    }

    if (source->supplied_mip_count > 0u) {
        for (uint32_t level = 0; level < source->supplied_mip_count; ++level) {
            const edds_decoded_mip *mip = &source->supplied_mips[level];

            for (size_t at = 3; at < (size_t)mip->width * mip->height * 4u; at += 4u) {
                if (mip->rgba[at] != 255u) {
                    return EDDS_ALPHA_USED;
                }
            }
        }

        return EDDS_ALPHA_OPAQUE;
    }

    for (size_t at = 3; at < (size_t)source->width * source->height * 4u; at += 4u) {
        if (source->rgba[at] != 255u) {
            return EDDS_ALPHA_USED;
        }
    }

    return EDDS_ALPHA_OPAQUE;
}

/**
 * Everything after decoding: one source image, whatever produced it, becomes one EDDS.
 * The steps: the mip chain, the swizzle, the runtime format, the container, the file; progress is
 * reported between them. Returns EDDS_OK, or the first refusal with `error` filled in.
 */
static edds_status encode_image(
    const edds_decoded_source *image,
    FILE                      *output,
    const edds_profile        *profile,
    edds_cancelled_fn          cancelled,
    void                      *cancel_context,
    edds_progress_fn           progress,
    void                      *progress_context,
    edds_error                *error) {
    /* The mip chain and how many levels it has; the runtime format, by the alpha really used. */
    generated_mip           mips[EDDS_MAX_MIPS];
    const edds_pixel_format format = edds_profile_pixel_format(profile, source_alpha_of(image));
    uint32_t                count  = 0;
    edds_status             status;

    report(progress, progress_context, 0.15);
    status = generate_mips(image, profile, mips, &count, cancelled, cancel_context, error);

    if (status == EDDS_OK) {
        /* Byte-domain swizzling follows filtering/normalization, before pixel encoding. */
        for (uint32_t level = 0; level < count; ++level) {
            for (size_t at = 0; at < mips[level].bytes; at += 4u) {
                uint8_t      *p   = mips[level].bgra + at;
                const uint8_t red = p[2], alpha = p[3];

                /*
                 * The bytes are blue, green, red and alpha. The NORMAL_NOHQ, NORMAL_GA and
                 * TERRAIN_NORMAL swizzles change them only with the None or the DXT conversion.
                 */
                switch (profile->swizzling) {
                    case EDDS_SWIZZLE_ALPHA_TO_RGB:
                        p[0] = p[1] = p[2] = alpha;
                        p[3]               = 255;
                        break;

                    case EDDS_SWIZZLE_SMDI_TO_GS:
                        p[2] = p[0];
                        p[0] = 0;
                        p[3] = 255;
                        break;

                    case EDDS_SWIZZLE_NORMAL_NOHQ:
                        if (profile->conversion != EDDS_CONVERSION_NONE && profile->conversion != EDDS_CONVERSION_DXT) {
                            break;
                        }

                        p[0] = p[2] = 0;
                        p[3]        = (uint8_t)(255u - red);
                        break;

                    case EDDS_SWIZZLE_NORMAL_GA:
                        if (profile->conversion != EDDS_CONVERSION_NONE && profile->conversion != EDDS_CONVERSION_DXT) {
                            break;
                        }

                        p[0] = p[2] = 0;
                        p[3]        = red;
                        break;

                    case EDDS_SWIZZLE_TERRAIN_NORMAL:
                        if (profile->conversion != EDDS_CONVERSION_NONE && profile->conversion != EDDS_CONVERSION_DXT) {
                            break;
                        }

                        p[0] = 0;
                        p[2] = alpha;
                        p[3] = red;
                        break;

                    default: break;
                }
            }
        }

        report(progress, progress_context, 0.30);
        status = encode_mips(mips, count, format, profile->conversion_quality, cancelled, cancel_context, error);
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

    return status;
}

/**
 * Checks the arguments, the profile and the size, then runs the image through encode_image as a
 * decoded source would be. Returns EDDS_OK, or the first refusal with `error` filled in.
 */
edds_status edds_encode_rgba(
    const uint8_t      *rgba,
    uint32_t            width,
    uint32_t            height,
    int                 has_alpha,
    FILE               *output,
    const edds_profile *profile,
    edds_cancelled_fn   cancelled,
    void               *cancel_context,
    edds_error         *error) {
    edds_decoded_source image = { 0 };
    edds_status         status;

    if (rgba == NULL || output == NULL || profile == NULL) {
        edds_fail(error, "invalid-api-argument", "The pixels, output, and profile are required.");
        return EDDS_INTERNAL_FAILURE;
    }

    status = edds_profile_check(profile, error);

    if (status != EDDS_OK) {
        return status;
    }

    if (profile->contains_mips) {
        edds_fail(error, "unsupported-combination", "ContainsMips=true requires a DDS source with a controlled supplied-mip layout.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (width == 0 ||
        height == 0 ||
        width > EDDS_MAX_DIMENSION ||
        height > EDDS_MAX_DIMENSION ||
        !edds_decoded_size_allowed(width, height)) {
        edds_fail(error, "image-size-limit", "The image dimensions are outside the supported limits.");
        return EDDS_INVALID_INPUT;
    }

    if (cancelled != NULL && cancelled(cancel_context)) {
        edds_fail(error, "cancelled", "The conversion was cancelled.");
        return EDDS_CANCELLED;
    }

    image.width     = width;
    image.height    = height;
    image.has_alpha = has_alpha != 0;
    /* Read only: the pipeline copies the top level before it changes a sample. */
    image.rgba      = (uint8_t *)(uintptr_t)rgba;

    return encode_image(&image, output, profile, cancelled, cancel_context, NULL, NULL, error);
}

/**
 * Checks the arguments, the profile and the source format, decodes the source with the decoder
 * of its format, and runs the image through encode_image into `output`. Returns EDDS_OK, or the
 * first refusal with `error` filled in.
 */
edds_status edds_convert(
    FILE               *source,
    edds_source_format  source_format,
    FILE               *output,
    const edds_profile *profile,
    edds_cancelled_fn   cancelled,
    void               *cancel_context,
    edds_progress_fn    progress,
    void               *progress_context,
    edds_error         *error) {
    edds_decoded_source image = { 0 };
    edds_status         status;

    if (source == NULL || output == NULL || profile == NULL) {
        edds_fail(error, "invalid-api-argument", "The source, output, and profile are required.");
        return EDDS_INTERNAL_FAILURE;
    }

    status = edds_profile_check(profile, error);

    if (status != EDDS_OK) {
        return status;
    }

    if (edds_source_capability_of_format(source_format) == NULL) {
        edds_fail(error, "unsupported-source-format", "The source format is outside the supported Workbench resource classes.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    if (source_format != EDDS_SOURCE_DDS && profile->contains_mips) {
        edds_fail(error, "unsupported-combination", "ContainsMips=true requires a DDS source with a controlled supplied-mip layout.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    status = EDDS_UNSUPPORTED_FORMAT;

    /*
     * Deliberately without a default: the capability table above has already refused anything
     * outside the enum, so a format added to the contract with no decoder behind it is a build
     * error here rather than a run that refuses what the rest of the product advertises.
     */
    switch (source_format) {
        case EDDS_SOURCE_PNG:  status = edds_decode_png(source, &image, error); break;
        case EDDS_SOURCE_TGA:  status = edds_decode_tga(source, &image, error); break;
        case EDDS_SOURCE_JPG:  status = edds_decode_jpeg(source, &image, error); break;
        case EDDS_SOURCE_TIFF: status = edds_decode_tiff(source, &image, error); break;
        case EDDS_SOURCE_DDS:  status = edds_decode_dds(source, &image, error); break;
    }

    if (status != EDDS_OK) {
        return status;
    }

    status = encode_image(&image, output, profile, cancelled, cancel_context, progress, progress_context, error);
    free_source(&image);

    return status;
}
