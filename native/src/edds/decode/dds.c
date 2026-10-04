#include "memory.h"
#include "image.h"
#include "gpu.h"

#include <stdlib.h>
#include <string.h>

/** The sizes, flag bits and format codes of the DDS header that this decoder checks. */
enum {
    /** Bytes in the header, its "DDS " signature included, and in the optional DX10 header. */
    DDS_HEADER_BYTES      = 128,
    DDS_DX10_HEADER_BYTES = 20,

    /** Bits of the header flags, at byte 8. */
    DDSD_CAPS        = 0x00000001,
    DDSD_HEIGHT      = 0x00000002,
    DDSD_WIDTH       = 0x00000004,
    DDSD_PITCH       = 0x00000008,
    DDSD_PIXELFORMAT = 0x00001000,
    DDSD_MIPMAPCOUNT = 0x00020000,
    DDSD_LINEARSIZE  = 0x00080000,

    /** Bits of the pixel format flags, at byte 80. */
    DDPF_ALPHAPIXELS = 0x00000001,
    DDPF_FOURCC      = 0x00000004,
    DDPF_RGB         = 0x00000040,

    /** Bits of the capability flags, at byte 108. */
    DDSCAPS_COMPLEX = 0x00000008,
    DDSCAPS_TEXTURE = 0x00001000,
    DDSCAPS_MIPMAP  = 0x00400000,

    /** The resource dimension of a 2D texture, at byte 132 in the DX10 header. */
    DDS_RESOURCE_DIMENSION_TEXTURE2D = 3,

    /** The DXGI format codes, at byte 128 in the DX10 header, that dx10_format knows. */
    DXGI_FORMAT_R8G8_UNORM     = 49,
    DXGI_FORMAT_R8_UNORM       = 61,
    DXGI_FORMAT_BC1_UNORM      = 71,
    DXGI_FORMAT_BC3_UNORM      = 77,
    DXGI_FORMAT_BC4_UNORM      = 80,
    DXGI_FORMAT_BC5_UNORM      = 83,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_B8G8R8X8_UNORM = 88,
    DXGI_FORMAT_BC7_UNORM      = 98
};

/** Mip `level`'s width or height, from the top level's `base`: halved per level, at least 1. */
static uint32_t mip_dimension(uint32_t base, uint32_t level) {
    while (level-- > 0 && base > 1) {
        base /= 2u;
    }

    return base;
}

/** The number of levels in a complete chain, from width x height down to 1 x 1. */
static uint32_t complete_mip_count(uint32_t width, uint32_t height) {
    uint32_t count = 1;

    while (width > 1 || height > 1) {
        width  = width > 1 ? width / 2u : 1u;
        height = height > 1 ? height / 2u : 1u;
        ++count;
    }

    return count;
}

/** 1 when all `size` bytes are zero, 0 otherwise. */
static int all_zero(const uint8_t *bytes, size_t size) {
    for (size_t at = 0; at < size; ++at) {
        if (bytes[at] != 0) {
            return 0;
        }
    }

    return 1;
}

/**
 * The pixel format of a file without a DX10 header, read from the pixel format flags at byte 80:
 * DXT1 or DXT5 by four-character code, or 32-bit BGRA or BGRX by bit masks. Anything else is
 * EDDS_PIXEL_UNKNOWN. `*declares_alpha` is set for DXT5 and BGRA.
 */
static edds_pixel_format legacy_format(const uint8_t *header, int *declares_alpha) {
    const uint32_t flags = edds_u32le(header + 80);

    *declares_alpha = 0;

    /* A four-character code at byte 84, with nothing else set and the 20 bytes after it zero. */
    if ((flags & DDPF_FOURCC) != 0) {
        if (flags != DDPF_FOURCC || !all_zero(header + 88, 20)) {
            return EDDS_PIXEL_UNKNOWN;
        }

        if (memcmp(header + 84, "DXT1", 4) == 0) {
            return EDDS_PIXEL_DXT1;
        }

        if (memcmp(header + 84, "DXT5", 4) == 0) {
            *declares_alpha = 1;
            return EDDS_PIXEL_DXT5;
        }

        return EDDS_PIXEL_UNKNOWN;
    }

    /*
     * Otherwise the words at bytes 88 to 100 must be exactly those of 32-bit BGRA or BGRX, and the
     * word at byte 104, the alpha mask, must match the DDPF_ALPHAPIXELS flag.
     */
    if ((flags != DDPF_RGB && flags != (DDPF_RGB | DDPF_ALPHAPIXELS)) ||
        edds_u32le(header + 88) != 32u ||
        edds_u32le(header + 92) != 0x00ff0000u ||
        edds_u32le(header + 96) != 0x0000ff00u ||
        edds_u32le(header + 100) != 0x000000ffu) {
        return EDDS_PIXEL_UNKNOWN;
    }

    if ((flags & DDPF_ALPHAPIXELS) != 0 && edds_u32le(header + 104) == 0xff000000u) {
        *declares_alpha = 1;
        return EDDS_PIXEL_BGRA8;
    }

    return (flags & DDPF_ALPHAPIXELS) == 0 && edds_u32le(header + 104) == 0 ? EDDS_PIXEL_BGRX8 : EDDS_PIXEL_UNKNOWN;
}

/**
 * The pixel format a DX10 header's DXGI format code stands for, or EDDS_PIXEL_UNKNOWN.
 * `*declares_alpha` is set for the formats that carry alpha: BC3 (DXT5), BGRA and BC7.
 */
static edds_pixel_format dx10_format(uint32_t dxgi, int *declares_alpha) {
    *declares_alpha = 0;

    switch (dxgi) {
        case DXGI_FORMAT_R8_UNORM:       return EDDS_PIXEL_R8;
        case DXGI_FORMAT_R8G8_UNORM:     return EDDS_PIXEL_RG8;
        case DXGI_FORMAT_BC1_UNORM:      return EDDS_PIXEL_DXT1;
        case DXGI_FORMAT_BC3_UNORM:      *declares_alpha = 1; return EDDS_PIXEL_DXT5;
        case DXGI_FORMAT_BC4_UNORM:      return EDDS_PIXEL_BC4;
        case DXGI_FORMAT_BC5_UNORM:      return EDDS_PIXEL_BC5;
        case DXGI_FORMAT_B8G8R8A8_UNORM: *declares_alpha = 1; return EDDS_PIXEL_BGRA8;
        case DXGI_FORMAT_B8G8R8X8_UNORM: return EDDS_PIXEL_BGRX8;
        case DXGI_FORMAT_BC7_UNORM:      *declares_alpha = 1; return EDDS_PIXEL_BC7;

        default: return EDDS_PIXEL_UNKNOWN;
    }
}

/** Frees every mip decoded so far, and leaves `image` with no mips and no RGBA. */
static void release_mips(edds_decoded_source *image) {
    for (uint32_t level = 0; level < image->supplied_mip_count; ++level) {
        edds_free(image->supplied_mips[level].rgba);
        image->supplied_mips[level].rgba = NULL;
    }

    image->rgba               = NULL;
    image->supplied_mip_count = 0;
}

/**
 * Decodes a whole DDS file: one 2D texture, either its top level alone or its complete mip chain,
 * in a format legacy_format or dx10_format knows. Every level is decoded into its own RGBA buffer
 * in image->supplied_mips, and image->rgba is the top level's. On success the caller owns the
 * mips; on failure nothing is left allocated.
 */
edds_status edds_decode_dds(FILE *input, edds_decoded_source *image, edds_error *error) {
    /* The whole file. */
    uint8_t *bytes = NULL;
    size_t   size  = 0;

    /* What the header says: the top level's size, the mip count, the flags and capability bits. */
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    uint32_t flags;
    uint32_t caps;

    /* The size of the headers, the DX10 one included, and so where the first mip starts. */
    uint32_t header_bytes = DDS_HEADER_BYTES;

    /* The pixel format, and whether it carries alpha. */
    edds_pixel_format format;
    int               declares_alpha = 0;

    /* Where the next mip starts in the file. */
    uint64_t payload_at;

    if (!edds_read_all(input, &bytes, &size, error)) {
        return EDDS_INVALID_INPUT;
    }

    if (size < DDS_HEADER_BYTES) {
        edds_fail(error, "truncated-dds-header", "The DDS source header is truncated.");
        goto invalid;
    }

    /* The "DDS " signature, then the sizes the header records: 124 at byte 4 and 32 at byte 76. */
    if (memcmp(bytes, "DDS ", 4) != 0 || edds_u32le(bytes + 4) != 124u || edds_u32le(bytes + 76) != 32u) {
        edds_fail(error, "invalid-dds-header", "The DDS source does not have the required header sizes.");
        goto invalid;
    }

    if (memcmp(bytes + 36, "ENF1", 4) == 0) {
        edds_fail(error, "unsupported-dds-container", "An ENF1 EDDS is a runtime texture, not a standard DDS source.");
        goto unsupported;
    }

    /* The reserved bytes, 32 to 75 and 116 to 127, must be zero. */
    if (!all_zero(bytes + 32, 44) || edds_u32le(bytes + 116) != 0 || edds_u32le(bytes + 120) != 0 || edds_u32le(bytes + 124) != 0) {
        edds_fail(error, "unsupported-dds-header", "The DDS source uses reserved or legacy header fields outside the controlled layout.");
        goto unsupported;
    }

    flags     = edds_u32le(bytes + 8);
    width     = edds_u32le(bytes + 16);
    height    = edds_u32le(bytes + 12);
    mip_count = edds_u32le(bytes + 28);
    caps      = edds_u32le(bytes + 108);

    if (width == 0 || height == 0 || width > EDDS_MAX_DIMENSION || height > EDDS_MAX_DIMENSION) {
        edds_fail(error, "dds-dimension-limit", "DDS dimensions must be between 1 and %u.", EDDS_MAX_DIMENSION);
        goto invalid;
    }

    /*
     * One two-dimensional texture. The header flags must include caps, height, width and pixel
     * format, nothing outside the DDSD_ bits, and exactly one of pitch and linear size. The
     * capability bits must include TEXTURE, with nothing but COMPLEX and MIPMAP beside it. The
     * words at bytes 24 and 112 must be zero.
     */
    if ((flags & (DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT)) !=
            (DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT) ||
        (flags &
            ~(DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT |
                DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE)) != 0 ||
        ((flags & DDSD_PITCH) != 0) == ((flags & DDSD_LINEARSIZE) != 0) ||
        (caps & DDSCAPS_TEXTURE) == 0 ||
        edds_u32le(bytes + 24) != 0 ||
        (caps & ~(DDSCAPS_TEXTURE | DDSCAPS_COMPLEX | DDSCAPS_MIPMAP)) != 0 ||
        edds_u32le(bytes + 112) != 0) {
        edds_fail(error, "unsupported-dds-topology", "Only one two-dimensional DDS texture surface is supported as input.");
        goto unsupported;
    }

    /* A mip count of 0 counts as the top level alone. */
    if (mip_count == 0) {
        mip_count = 1;
    }

    /*
     * The mips: the top level alone, with no mip count flag and no capability bit but TEXTURE, or
     * the complete chain down to 1 x 1, with the mip count flag and the COMPLEX and MIPMAP bits.
     */
    if (mip_count > EDDS_MAX_MIPS ||
        (mip_count != 1u && mip_count != complete_mip_count(width, height)) ||
        (mip_count > 1 &&
            ((flags & DDSD_MIPMAPCOUNT) == 0 ||
                (caps & (DDSCAPS_COMPLEX | DDSCAPS_MIPMAP)) !=
                    (DDSCAPS_COMPLEX | DDSCAPS_MIPMAP))) ||
        (mip_count == 1u && ((flags & DDSD_MIPMAPCOUNT) != 0 || caps != DDSCAPS_TEXTURE))) {
        edds_fail(error, "unsupported-dds-mip-layout",
            "DDS input must contain either its top level or one complete largest-to-smallest chain.");
        goto unsupported;
    }

    /*
     * The pixel format. A four-character code of DX10 means a DX10 header follows the header and
     * names the format by DXGI code; otherwise the header's pixel format flags describe it.
     */
    if ((edds_u32le(bytes + 80) & DDPF_FOURCC) != 0 && memcmp(bytes + 84, "DX10", 4) == 0) {
        uint32_t dxgi;

        if (size < DDS_HEADER_BYTES + DDS_DX10_HEADER_BYTES) {
            edds_fail(error, "truncated-dds-dx10-header", "The DDS DX10 source header is truncated.");
            goto invalid;
        }

        if (edds_u32le(bytes + 80) != DDPF_FOURCC || !all_zero(bytes + 88, 20)) {
            edds_fail(error, "unsupported-dds-format", "The DDS DX10 pixel descriptor contains legacy fields.");
            goto unsupported;
        }

        dxgi = edds_u32le(bytes + 128);

        /* One 2D resource: the dimension at byte 132, 0 at bytes 136 and 144, and 1 at 140. */
        if (edds_u32le(bytes + 132) != DDS_RESOURCE_DIMENSION_TEXTURE2D ||
            edds_u32le(bytes + 136) != 0 ||
            edds_u32le(bytes + 140) != 1 ||
            edds_u32le(bytes + 144) != 0) {
            edds_fail(error, "unsupported-dds-topology", "DDS arrays, cubes, volumes, and non-2D resources are not supported as input.");
            goto unsupported;
        }

        format        = dx10_format(dxgi, &declares_alpha);
        header_bytes += DDS_DX10_HEADER_BYTES;
    } else {
        format = legacy_format(bytes, &declares_alpha);
    }

    if (format == EDDS_PIXEL_UNKNOWN) {
        edds_fail(error, "unsupported-dds-format", "The DDS pixel format is recognized as outside the controlled LDR input matrix.");
        goto unsupported;
    }

    /*
     * The word at byte 20 must be the tight size of the top level: for a format of separate pixels,
     * the bytes of one row, under DDSD_PITCH; for a block format, the bytes of the whole level,
     * under DDSD_LINEARSIZE.
     */
    {
        const uint32_t top_bytes      = edds_gpu_mip_bytes(format, width, height);
        const uint32_t block          = edds_gpu_block_bytes(format);
        const uint32_t required_flag  = block == 0 ? DDSD_PITCH : DDSD_LINEARSIZE;
        const uint32_t expected_pitch = block == 0 ? width * edds_gpu_pixel_bytes(format) : top_bytes;

        if (top_bytes == 0 || (flags & required_flag) == 0 || edds_u32le(bytes + 20) != expected_pitch) {
            edds_fail(error, "unsupported-dds-pitch", "The DDS source must use the tight pitch or top-level linear size of its format.");
            goto unsupported;
        }
    }

    /* The mips follow the headers, the largest first, and each is decoded into its own buffer. */
    payload_at = header_bytes;
    memset(image, 0, sizeof *image);
    image->width     = width;
    image->height    = height;
    image->has_alpha = declares_alpha;

    for (uint32_t level = 0; level < mip_count; ++level) {
        edds_decoded_mip *mip          = &image->supplied_mips[level];
        const uint32_t    mip_width    = mip_dimension(width, level);
        const uint32_t    mip_height   = mip_dimension(height, level);
        const uint32_t    stored_bytes = edds_gpu_mip_bytes(format, mip_width, mip_height);
        const uint64_t    rgba_bytes   = (uint64_t)mip_width * mip_height * 4u;

        if (stored_bytes == 0 || rgba_bytes > EDDS_MAX_PREVIEW_BYTES || payload_at > size || stored_bytes > size - payload_at) {
            edds_fail(error, "truncated-dds-mip", "DDS mip %u is truncated or exceeds the decoded-image limit.", level);
            goto decoded_invalid;
        }

        mip->rgba = edds_alloc((size_t)rgba_bytes);

        if (mip->rgba == NULL) {
            edds_fail(error, "allocation-failed", "Memory for DDS mip %u could not be allocated.", level);
            goto decoded_internal;
        }

        mip->width  = mip_width;
        mip->height = mip_height;

        /* Counted before it is decoded, so that release_mips frees it if the decode fails. */
        image->supplied_mip_count = level + 1u;

        if (!edds_gpu_decode(format, bytes + payload_at, stored_bytes, mip_width, mip_height, mip->rgba)) {
            edds_fail(error, "malformed-dds-mip", "DDS mip %u could not be decoded.", level);
            goto decoded_invalid;
        }

        payload_at += stored_bytes;
    }

    if (payload_at != size) {
        edds_fail(error, "trailing-dds-data", "The DDS source has bytes after its complete mip chain.");
        goto decoded_invalid;
    }

    image->rgba = image->supplied_mips[0].rgba;

    /*
     * DXT1 declares no alpha, yet its decoded pixels need not all be opaque: the image has alpha
     * when any pixel of any mip is not.
     */
    if (!image->has_alpha && format == EDDS_PIXEL_DXT1) {
        for (uint32_t level = 0; level < mip_count; ++level) {
            const edds_decoded_mip *mip    = &image->supplied_mips[level];
            const size_t            pixels = (size_t)mip->width * mip->height;

            for (size_t pixel = 0; pixel < pixels; ++pixel) {
                if (mip->rgba[pixel * 4u + 3u] != 255u) {
                    image->has_alpha = 1;
                }
            }
        }
    }

    /* The mips stay with the caller; only the file is freed. */
    edds_free(bytes);
    return EDDS_OK;

    /*
     * The failures. Once mips have been decoded, they are freed as well as the file;
     * decoded_invalid runs on into invalid.
     */
decoded_internal:
    release_mips(image);
    edds_free(bytes);
    return EDDS_INTERNAL_FAILURE;

decoded_invalid:
    release_mips(image);

invalid:
    edds_free(bytes);
    return EDDS_INVALID_INPUT;

unsupported:
    edds_free(bytes);
    return EDDS_UNSUPPORTED_FORMAT;
}
