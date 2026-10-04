/*
 * The fixtures fixture.h declares, each built byte by byte. An EDDS fixture is a DDS header that
 * carries the ENF1 marker, then the mip table, 8 bytes per mip (the container, COPY or "LZ4 ", and
 * the stored size) with the smallest mip first, then the stored mips in the order of the table.
 */
#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    /** The size of a DDS header, its "DDS " magic included. */
    DDS_HEADER_BYTES = 128
};

/** Writes a 32-bit value little-endian: the lowest byte first. */
static void put_u32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

/** Writes a 32-bit value big-endian: the highest byte first. */
static void put_u32be(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

/** Writes a 16-bit value big-endian: the high byte first. */
static void put_u16be(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

/**
 * The CRC-32 that closes a PNG chunk, taken over its type and data: the reflected polynomial
 * 0xEDB88320, one bit at a time.
 */
static uint32_t crc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = 0xffffffffu;

    for (size_t at = 0; at < size; ++at) {
        crc ^= bytes[at];

        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }

    return ~crc;
}

/**
 * The Adler-32 that closes a zlib stream: a running sum of the bytes and a running sum of those
 * sums, both modulo 65521, the second in the high 16 bits.
 */
static uint32_t adler32(const uint8_t *bytes, size_t size) {
    uint32_t first  = 1;
    uint32_t second = 0;

    for (size_t at = 0; at < size; ++at) {
        first  = (first + bytes[at]) % 65521u;
        second = (second + first) % 65521u;
    }

    return (second << 16) | first;
}

/**
 * Writes one PNG chunk at `output`: the data's length, the four-letter type, the data and the CRC.
 * Returns the bytes written, 12 more than the data.
 */
static size_t png_chunk(uint8_t *output, const char type[4], const uint8_t *data, uint32_t size) {
    put_u32be(output, size);
    memcpy(output + 4, type, 4);

    if (size != 0) {
        memcpy(output + 8, data, size);
    }

    put_u32be(output + 8u + size, crc32(output + 4, 4u + size));

    return 12u + size;
}

/**
 * Writes the 128-byte header of an EDDS fixture into `bytes`: the size, the mip count, the ENF1
 * marker and the pixel format. An empty `four_cc` (four zero bytes) gives 32-bit BGR masks, BGRA
 * when `alpha_mask` is set and BGRX when it is 0; any other `four_cc` names the format itself.
 */
static void header(uint8_t *bytes, uint32_t width, uint32_t height, uint32_t mips, const char four_cc[4], uint32_t alpha_mask) {
    /*
     * The magic, the size of the header after it (124), the flags (caps, height, width, pitch,
     * pixel format and mip count), the height, the width, the pitch of one row and the mip count.
     */
    memset(bytes, 0, DDS_HEADER_BYTES);
    memcpy(bytes, "DDS ", 4);
    put_u32(bytes + 4, 124);
    put_u32(bytes + 8, 0x0002100fu);
    put_u32(bytes + 12, height);
    put_u32(bytes + 16, width);
    put_u32(bytes + 20, width * 4u);
    put_u32(bytes + 28, mips);

    /* The marker that sets an EDDS apart from a standard DDS. */
    memcpy(bytes + 36, "ENF1", 4);

    /*
     * The pixel format, 32 bytes: its size, its flags (0x40 RGB, 0x41 RGB with alpha, 0x04 a
     * FourCC), the FourCC, and without one the bits per pixel and the red, green, blue and alpha
     * masks.
     */
    put_u32(bytes + 76, 32);
    put_u32(bytes + 80, four_cc[0] == '\0' ? (alpha_mask == 0 ? 0x40u : 0x41u) : 0x04u);
    memcpy(bytes + 84, four_cc, 4);
    put_u32(bytes + 88, four_cc[0] == '\0' ? 32u : 0u);
    put_u32(bytes + 92, four_cc[0] == '\0' ? 0x00ff0000u : 0u);
    put_u32(bytes + 96, four_cc[0] == '\0' ? 0x0000ff00u : 0u);
    put_u32(bytes + 100, four_cc[0] == '\0' ? 0x000000ffu : 0u);
    put_u32(bytes + 104, alpha_mask);

    /* The caps: a texture, and with more than one mip a complex one with mipmaps. */
    put_u32(bytes + 108, mips > 1u ? 0x00401008u : 0x00001000u);
}

/** A fixture of `size` zeroed bytes, or an empty one (NULL, 0) when the allocation failed. */
static test_bytes allocated(size_t size) {
    test_bytes fixture = { calloc(1, size), size };

    if (fixture.data == NULL) {
        fixture.size = 0;
    }

    return fixture;
}

/**
 * A 3x2 BGRA EDDS with two mips, both stored COPY: the table lists the 1x1 mip first and the 3x2
 * one second, and their bytes follow in the same order.
 */
test_bytes fixture_copy_bgra(void) {
    /* The 1x1 mip and the 3x2 one, four bytes a pixel. */
    static const uint8_t small[] = { 30, 20, 10, 40 };
    static const uint8_t large[] = {
        3, 2, 1, 4, 7, 6, 5, 8, 11, 10, 9, 12,
        15, 14, 13, 16, 19, 18, 17, 20, 23, 22, 21, 24
    };
    test_bytes fixture = allocated(DDS_HEADER_BYTES + 16u + sizeof small + sizeof large);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 3, 2, 2, "\0\0\0\0", 0xff000000u);

    /* The mip table: a COPY entry for each mip, with its stored size. */
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof small);
    memcpy(fixture.data + 136, "COPY", 4);
    put_u32(fixture.data + 140, (uint32_t)sizeof large);

    /* The mips themselves, as they are. */
    memcpy(fixture.data + 144, small, sizeof small);
    memcpy(fixture.data + 148, large, sizeof large);

    return fixture;
}

/**
 * Writes `length` bytes of `raw` into `output` as one LZ4 sequence of literals only, with no match
 * after them. Returns the bytes written.
 */
static size_t literal_block(uint8_t *output, const uint8_t *raw, size_t length) {
    size_t at        = 0;
    size_t remaining = length > 15u ? length - 15u : 0u;

    /* The token: the literal count in the high four bits, 15 when more of it follows. */
    output[at++] = (uint8_t)((length < 15u ? length : 15u) << 4);

    /* The rest of the count: a 255 for every full 255, then what is left. */
    if (length >= 15u) {
        while (remaining >= 255u) {
            output[at++]  = 255u;
            remaining    -= 255u;
        }

        output[at++] = (uint8_t)remaining;
    }

    memcpy(output + at, raw, length);

    return at + length;
}

/**
 * A 2x1 BGRX EDDS whose one mip is stored LZ4: the decoded size, then a single block, marked as
 * the last, that carries the eight bytes as literals.
 */
test_bytes fixture_lz4_bgrx(void) {
    /* The two pixels. */
    static const uint8_t raw[] = { 3, 2, 1, 0, 6, 5, 4, 17 };

    /*
     * The same bytes as an LZ4 block, and the stored size: the decoded size and the block's size,
     * 4 bytes each, and the block.
     */
    uint8_t        compressed[32];
    const size_t   compressed_size = literal_block(compressed, raw, sizeof raw);
    const uint32_t stored          = (uint32_t)(4u + 4u + compressed_size);
    test_bytes     fixture         = allocated(DDS_HEADER_BYTES + 8u + stored);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 2, 1, 1, "\0\0\0\0", 0u);

    /* The mip table's one entry. */
    memcpy(fixture.data + 128, "LZ4 ", 4);
    put_u32(fixture.data + 132, stored);

    /*
     * The stored mip: the decoded size, then the block's size with its top bit set to mark the
     * last block, then the block.
     */
    put_u32(fixture.data + 136, (uint32_t)sizeof raw);
    put_u32(fixture.data + 140, 0x80000000u | (uint32_t)compressed_size);
    memcpy(fixture.data + 144, compressed, compressed_size);

    return fixture;
}

/**
 * A 256x65 BGRX EDDS, every pixel the same, stored LZ4 in two blocks. The first holds the first
 * 65536 bytes as literals. The second, marked as the last, is one match of 1024 bytes from 4 bytes
 * back: it repeats the pixel before it, reaching back into what the first block decoded.
 */
test_bytes fixture_lz4_streaming_bgrx(void) {
    /* The whole mip in bytes, and the part the first block carries. */
    enum {
        RAW_BYTES   = 256 * 65 * 4,
        FIRST_BYTES = 65536
    };

    /* The mip, and the first block with room for its token and literal count. */
    uint8_t *raw   = malloc(RAW_BYTES);
    uint8_t *first = malloc(FIRST_BYTES + 300u);
    size_t   first_size;

    /*
     * The second block: a token with no literals, the offset 4, then the bytes that extend the
     * match length to 1024 (15 in the token, plus 255 + 255 + 255 + 240, plus the minimum of 4).
     */
    static const uint8_t second[] = { 0x0f, 0x04, 0x00, 0xff, 0xff, 0xff, 0xf0 };

    /* The fixture, the stored size of its mip, and where the next field goes. */
    test_bytes fixture;
    size_t     payload;
    size_t     at;

    if (raw == NULL || first == NULL) {
        free(raw);
        free(first);
        fixture.data = NULL;
        fixture.size = 0;
        return fixture;
    }

    /* Every pixel the same four bytes. */
    for (size_t index = 0; index < RAW_BYTES; index += 4u) {
        raw[index]      = 3;
        raw[index + 1u] = 2;
        raw[index + 2u] = 1;
        raw[index + 3u] = 99;
    }

    first_size = literal_block(first, raw, FIRST_BYTES);
    payload    = 4u + 4u + first_size + 4u + sizeof second;
    fixture    = allocated(DDS_HEADER_BYTES + 8u + payload);

    if (fixture.data != NULL) {
        header(fixture.data, 256, 65, 1, "\0\0\0\0", 0u);
        memcpy(fixture.data + 128, "LZ4 ", 4);
        put_u32(fixture.data + 132, (uint32_t)payload);

        /* The stored mip: the decoded size, then each block after its size, the last flagged. */
        at = 136;
        put_u32(fixture.data + at, RAW_BYTES);
        at += 4;
        put_u32(fixture.data + at, (uint32_t)first_size);
        at += 4;
        memcpy(fixture.data + at, first, first_size);
        at += first_size;
        put_u32(fixture.data + at, 0x80000000u | (uint32_t)sizeof second);
        at += 4;
        memcpy(fixture.data + at, second, sizeof second);
    }

    free(raw);
    free(first);
    return fixture;
}

/**
 * A 2x1 EDDS whose format comes from a DX10 header, DXGI format 93, with its one mip stored COPY.
 * The DX10 header's 20 bytes push the mip table to offset 148.
 */
test_bytes fixture_dx10_bgrx(void) {
    static const uint8_t raw[]   = { 30, 20, 10, 77, 60, 50, 40, 88 };
    test_bytes           fixture = allocated(DDS_HEADER_BYTES + 20u + 8u + sizeof raw);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 2, 1, 1, "DX10", 0u);

    /*
     * The DX10 header, five 4-byte fields: the DXGI format, a 2D texture (3), no flags, an array
     * of one, and a last field of 0.
     */
    put_u32(fixture.data + 128, 93u);
    put_u32(fixture.data + 132, 3u);
    put_u32(fixture.data + 136, 0u);
    put_u32(fixture.data + 140, 1u);
    put_u32(fixture.data + 144, 0u);

    /* The mip table's one entry, then the mip. */
    memcpy(fixture.data + 148, "COPY", 4);
    put_u32(fixture.data + 152, (uint32_t)sizeof raw);
    memcpy(fixture.data + 156, raw, sizeof raw);

    return fixture;
}

/** A 4x4 DXT1 EDDS: one 8-byte block of zeros, stored COPY. */
test_bytes fixture_dxt1(void) {
    static const uint8_t block[8] = { 0 };
    test_bytes           fixture  = allocated(DDS_HEADER_BYTES + 8u + sizeof block);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 4, 4, 1, "DXT1", 0u);
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof block);
    memcpy(fixture.data + 136, block, sizeof block);

    return fixture;
}

/** A 4x4 DXT5 EDDS of one 16-byte block, stored COPY: the alpha half, then the colour half. */
test_bytes fixture_dxt5_low_endpoints(void) {
    /* Opaque alpha, then black and red endpoints in the low-to-high order, every index the last. */
    static const uint8_t blocks[16] = {
        255, 255, 0, 0, 0, 0, 0, 0,
        0x00, 0x00, 0x00, 0xf8, 0xff, 0xff, 0xff, 0xff
    };
    test_bytes fixture = allocated(DDS_HEADER_BYTES + 8u + sizeof blocks);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 4, 4, 1, "DXT5", 0u);
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof blocks);
    memcpy(fixture.data + 136, blocks, sizeof blocks);

    return fixture;
}

/**
 * A standard 4x2 BGRX DDS with three levels: the EDDS header with its ENF1 marker cleared, then the
 * levels back to back, the largest first, with no mip table before them.
 */
test_bytes fixture_dds_bgrx_mips(void) {
    /* The levels, 4x2, 2x1 and 1x1, four bytes a pixel. */
    static const uint8_t top[] = {
        3, 2, 1, 0, 6, 5, 4, 0, 9, 8, 7, 0, 12, 11, 10, 0,
        15, 14, 13, 0, 18, 17, 16, 0, 21, 20, 19, 0, 24, 23, 22, 0
    };
    static const uint8_t middle[] = { 33, 22, 11, 0, 66, 55, 44, 0 };
    static const uint8_t last[]   = { 99, 88, 77, 0 };

    test_bytes fixture = allocated(DDS_HEADER_BYTES + sizeof top + sizeof middle + sizeof last);
    size_t     at      = DDS_HEADER_BYTES;

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 4, 2, 3, "\0\0\0\0", 0u);

    /*
     * Bytes 32 to 75 cleared, the ENF1 marker among them, and the pixel format written again:
     * 32-bit RGB with the red, green and blue masks.
     */
    memset(fixture.data + 32, 0, 44);
    put_u32(fixture.data + 76, 32);
    put_u32(fixture.data + 80, 0x40u);
    put_u32(fixture.data + 88, 32u);
    put_u32(fixture.data + 92, 0x00ff0000u);
    put_u32(fixture.data + 96, 0x0000ff00u);
    put_u32(fixture.data + 100, 0x000000ffu);

    /* The levels themselves, right after the header. */
    memcpy(fixture.data + at, top, sizeof top);
    at += sizeof top;
    memcpy(fixture.data + at, middle, sizeof middle);
    at += sizeof middle;
    memcpy(fixture.data + at, last, sizeof last);

    return fixture;
}

/**
 * The standard DDS of fixture_dds_bgrx_mips cut down to its top level: the size ends after the 4x2
 * level, and the flags, the mip count and the caps no longer speak of mips.
 */
test_bytes fixture_dds_bgrx_top(void) {
    test_bytes fixture = fixture_dds_bgrx_mips();

    if (fixture.data != NULL) {
        fixture.size = DDS_HEADER_BYTES + 4u * 2u * 4u;
        put_u32(fixture.data + 8, 0x0000100fu);
        put_u32(fixture.data + 28, 1u);
        put_u32(fixture.data + 108, 0x00001000u);
    }

    return fixture;
}

/**
 * A standard 4x4 DXT1 DDS with one level, an 8-byte block of zeros. Its flags name a linear size
 * instead of a pitch, and the linear size is the block's 8 bytes.
 */
test_bytes fixture_dds_dxt1_top(void) {
    static const uint8_t block[8] = { 0 };
    test_bytes           fixture  = allocated(DDS_HEADER_BYTES + sizeof block);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 4, 4, 1, "DXT1", 0u);
    put_u32(fixture.data + 8, 0x00081007u);
    put_u32(fixture.data + 20, (uint32_t)sizeof block);
    memset(fixture.data + 32, 0, 44);
    memcpy(fixture.data + DDS_HEADER_BYTES, block, sizeof block);

    return fixture;
}

/** The DXT5 counterpart of fixture_dds_dxt1_top: one 16-byte block of zeros. */
test_bytes fixture_dds_dxt5_top(void) {
    static const uint8_t block[16] = { 0 };
    test_bytes           fixture   = allocated(DDS_HEADER_BYTES + sizeof block);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 4, 4, 1, "DXT5", 0u);
    put_u32(fixture.data + 8, 0x00081007u);
    put_u32(fixture.data + 20, (uint32_t)sizeof block);
    memset(fixture.data + 32, 0, 44);
    memcpy(fixture.data + DDS_HEADER_BYTES, block, sizeof block);

    return fixture;
}

/**
 * A standard 4x2 BGRA DDS with three levels, laid out like fixture_dds_bgrx_mips, its alpha 255 in
 * the top level, 128 in the middle one and 64 in the last.
 */
test_bytes fixture_dds_bgra_alpha_mips(void) {
    /* The levels, 4x2, 2x1 and 1x1, four bytes a pixel. */
    static const uint8_t top[] = {
        3, 2, 1, 255, 6, 5, 4, 255, 9, 8, 7, 255, 12, 11, 10, 255,
        15, 14, 13, 255, 18, 17, 16, 255, 21, 20, 19, 255, 24, 23, 22, 255
    };
    static const uint8_t middle[] = { 33, 22, 11, 128, 66, 55, 44, 128 };
    static const uint8_t last[]   = { 99, 88, 77, 64 };

    test_bytes fixture = allocated(DDS_HEADER_BYTES + sizeof top + sizeof middle + sizeof last);
    size_t     at      = DDS_HEADER_BYTES;

    if (fixture.data == NULL) {
        return fixture;
    }

    /* The header with bytes 32 to 75 cleared, and the ENF1 marker with them. */
    header(fixture.data, 4, 2, 3, "\0\0\0\0", 0xff000000u);
    memset(fixture.data + 32, 0, 44);

    /* The levels themselves, right after the header. */
    memcpy(fixture.data + at, top, sizeof top);
    at += sizeof top;
    memcpy(fixture.data + at, middle, sizeof middle);
    at += sizeof middle;
    memcpy(fixture.data + at, last, sizeof last);

    return fixture;
}

/**
 * A standard 4x4 DDS with one level, in the DXGI format given, its payload copied in as it is. A
 * `bytes_per_pixel` of 0 means a block format: the header then states a linear size, the whole
 * payload, where an uncompressed format states the pitch of a four-pixel row.
 */
test_bytes fixture_dds_dx10_top(
    uint32_t       dxgi_format,
    const uint8_t *payload,
    size_t         payload_size,
    uint32_t       bytes_per_pixel) {
    test_bytes fixture    = allocated(DDS_HEADER_BYTES + 20u + payload_size);
    const int  compressed = bytes_per_pixel == 0u;

    if (fixture.data == NULL) {
        return fixture;
    }

    /* The DDS header: a linear size or a pitch, as above, and the ENF1 marker cleared. */
    header(fixture.data, 4, 4, 1, "DX10", 0u);
    put_u32(fixture.data + 8, compressed ? 0x00081007u : 0x0000100fu);
    put_u32(fixture.data + 20, compressed ? (uint32_t)payload_size : 4u * bytes_per_pixel);
    memset(fixture.data + 32, 0, 44);

    /* The DX10 header: the DXGI format, a 2D texture (3), an array of one. */
    put_u32(fixture.data + 128, dxgi_format);
    put_u32(fixture.data + 132, 3u);
    put_u32(fixture.data + 140, 1u);

    memcpy(fixture.data + DDS_HEADER_BYTES + 20u, payload, payload_size);

    return fixture;
}

/**
 * A standard 4x2 DDS with three levels in DXGI format 61, R8: one byte a pixel, so the pitch of a
 * row is 4.
 */
test_bytes fixture_dds_dx10_r8_mips(void) {
    /* The levels, 4x2, 2x1 and 1x1. */
    static const uint8_t top[]    = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint8_t middle[] = { 11, 22 };
    static const uint8_t last[]   = { 33 };

    test_bytes fixture = allocated(DDS_HEADER_BYTES + 20u + sizeof top + sizeof middle + sizeof last);
    size_t     at      = DDS_HEADER_BYTES + 20u;

    if (fixture.data == NULL) {
        return fixture;
    }

    /*
     * The DDS header with a pitch of 4 and its ENF1 marker cleared, then the DX10 header: the DXGI
     * format, a 2D texture (3), an array of one.
     */
    header(fixture.data, 4, 2, 3, "DX10", 0u);
    put_u32(fixture.data + 20, 4u);
    memset(fixture.data + 32, 0, 44);
    put_u32(fixture.data + 128, 61u);
    put_u32(fixture.data + 132, 3u);
    put_u32(fixture.data + 140, 1u);

    /* The levels themselves, right after the DX10 header. */
    memcpy(fixture.data + at, top, sizeof top);
    at += sizeof top;
    memcpy(fixture.data + at, middle, sizeof middle);
    at += sizeof middle;
    memcpy(fixture.data + at, last, sizeof last);

    return fixture;
}

/**
 * A 4x4 EDDS whose FourCC is four odd bytes: a letter, a double quote, a backslash and the byte 1.
 * Its one mip, an 8-byte block of zeros, is stored COPY.
 */
test_bytes fixture_odd_fourcc(void) {
    static const char    odd_fourcc[4] = { 'Q', '"', '\\', '\1' };
    static const uint8_t block[8]      = { 0 };
    test_bytes           fixture       = allocated(DDS_HEADER_BYTES + 8u + sizeof block);

    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 4, 4, 1, odd_fourcc, 0u);
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof block);
    memcpy(fixture.data + 136, block, sizeof block);

    return fixture;
}

/** The COPY fixture with its height and width, bytes 12 to 19, set to 0xffffffff each. */
test_bytes fixture_integer_overflow(void) {
    test_bytes fixture = fixture_copy_bgra();

    if (fixture.data != NULL) {
        memset(fixture.data + 12, 0xff, 8);
    }

    return fixture;
}

/**
 * Sets the low `count` bits of `value` in `bytes` from bit `*bit` on, the lowest first, and moves
 * `*bit` past them. The bits are only ever set, so the buffer starts zeroed.
 */
static void fixture_bits(uint8_t *bytes, size_t *bit, unsigned value, unsigned count) {
    for (unsigned at = 0; at < count; ++at, ++*bit) {
        bytes[*bit / 8u] |= (uint8_t)(((value >> at) & 1u) << (*bit % 8u));
    }
}

/**
 * A `side` by `side` RGBA PNG whose every byte is zero, for a side of 1 to 4096, its zlib stream
 * compressed for real by one fixed-Huffman block. Any other side gives an empty fixture.
 */
test_bytes fixture_png_flat(uint32_t side) {
    /*
     * The scanlines, a filter byte and four bytes a pixel each, and room for their stream: two
     * bytes for every run of 258 zeros, and 2048 to spare.
     */
    const uint32_t filtered = (side * 4u + 1u) * side;
    const size_t   capacity = (size_t)filtered / 258u * 2u + 2048u;

    /* The PNG, the zlib stream that goes into its IDAT chunk, and the 13 bytes of its IHDR. */
    test_bytes fixture;
    uint8_t   *idat;
    uint8_t    ihdr[13] = { 0 };

    /*
     * Where the next bit of the stream goes, after the 2-byte zlib header; where the next chunk
     * goes, after the 8-byte signature; and the zeros still to write after the first one.
     */
    size_t   bit       = 16u;
    size_t   at        = 8u;
    uint32_t remaining = filtered - 1u;

    if (side == 0u || side > 4096u) {
        return (test_bytes){ NULL, 0 };
    }

    fixture = allocated(capacity + 64u);
    idat    = calloc(1u, capacity);

    if (fixture.data == NULL || idat == NULL) {
        fixture_free(fixture);
        free(idat);
        return (test_bytes){ NULL, 0 };
    }

    /* The zlib header. */
    idat[0] = 0x78;
    idat[1] = 0x01;

    /* Owned fixed-Huffman Deflate: one zero, then distance-one runs, without a codec dependency. */
    fixture_bits(idat, &bit, 3u, 3u);  /* Final block, fixed Huffman. */
    fixture_bits(idat, &bit, 12u, 8u); /* Literal zero. */

    while (remaining >= 258u) {
        fixture_bits(idat, &bit, 163u, 8u); /* Length 258. */
        fixture_bits(idat, &bit, 0u, 5u);   /* Distance one. */
        remaining -= 258u;
    }

    /* Fewer than 258 zeros left: a literal zero for each. */
    while (remaining-- > 0u) {
        fixture_bits(idat, &bit, 12u, 8u);
    }

    fixture_bits(idat, &bit, 0u, 7u); /* End of block. */

    /*
     * The Adler-32 of the zeros, from the next whole byte: the first sum stays 1 and the second
     * grows by 1 a byte.
     */
    put_u32be(idat + (bit + 7u) / 8u, ((filtered % 65521u) << 16) | 1u);

    /* The PNG: the signature, then IHDR (side by side, 8 bits, colour type 6, RGBA). */
    memcpy(fixture.data, "\x89PNG\r\n\x1a\n", 8u);
    put_u32be(ihdr, side);
    put_u32be(ihdr + 4u, side);
    ihdr[8] = 8u;
    ihdr[9] = 6u;

    /* The chunks: IHDR, the stream with its Adler-32 in one IDAT, and IEND. */
    at           += png_chunk(fixture.data + at, "IHDR", ihdr, sizeof ihdr);
    at           += png_chunk(fixture.data + at, "IDAT", idat, (uint32_t)((bit + 7u) / 8u + 4u));
    at           += png_chunk(fixture.data + at, "IEND", NULL, 0u);
    fixture.size  = at;

    free(idat);
    return fixture;
}

/**
 * A 3x2 RGBA PNG, every scanline with filter 0, its zlib stream one Deflate block of type 0, which
 * holds the bytes as they are.
 */
test_bytes fixture_png_rgba(void) {
    /* The two scanlines, each its filter byte, 0, and three RGBA pixels. */
    static const uint8_t filtered[] = {
        0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
        0, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220
    };

    /*
     * The IHDR data; the zlib stream (the 2-byte header, the block's 5 bytes of header, the
     * scanlines, the 4-byte Adler-32); and the file: the signature, IHDR, IDAT and IEND.
     */
    uint8_t    ihdr[13] = { 0 };
    uint8_t    idat[2u + 5u + sizeof filtered + 4u];
    test_bytes fixture = allocated(8u + 25u + 12u + sizeof idat + 12u);
    size_t     at      = 0;

    if (fixture.data == NULL) {
        return fixture;
    }

    /* The signature, then IHDR: 3 by 2, 8 bits, colour type 6 (RGBA). */
    memcpy(fixture.data + at, "\x89PNG\r\n\x1a\n", 8);
    at += 8;
    put_u32be(ihdr, 3);
    put_u32be(ihdr + 4, 2);
    ihdr[8]  = 8;
    ihdr[9]  = 6;
    at      += png_chunk(fixture.data + at, "IHDR", ihdr, sizeof ihdr);

    /*
     * The zlib stream: the header; 1 for the last block, of type 0; the length and the same length
     * with every bit flipped, low byte first; the scanlines; their Adler-32.
     */
    idat[0] = 0x78;
    idat[1] = 0x01;
    idat[2] = 0x01;
    idat[3] = (uint8_t)sizeof filtered;
    idat[4] = 0;
    idat[5] = (uint8_t)(~(uint32_t)sizeof filtered & 0xffu);
    idat[6] = 0xff;
    memcpy(idat + 7, filtered, sizeof filtered);
    put_u32be(idat + 7u + sizeof filtered, adler32(filtered, sizeof filtered));

    at           += png_chunk(fixture.data + at, "IDAT", idat, sizeof idat);
    at           += png_chunk(fixture.data + at, "IEND", NULL, 0);
    fixture.size  = at;

    return fixture;
}

/**
 * A 2x1 RGB PNG with a palette and a tRNS colour key, its zlib stream (built as in
 * fixture_png_rgba) spread over one IDAT chunk per byte.
 */
test_bytes fixture_png_rgb_keyed(void) {
    /* Two RGB pixels, (10,20,30) and (40,50,60); the first is the transparent key. */
    static const uint8_t filtered[] = { 0, 10, 20, 30, 40, 50, 60 };
    /* A palette of two colours, and the key colour as three 16-bit samples, high byte first. */
    static const uint8_t palette[]  = { 1, 2, 3, 4, 5, 6 };
    static const uint8_t key[]      = { 0, 10, 0, 20, 0, 30 };

    /*
     * The IHDR data, the zlib stream, and the file: the signature, IHDR, PLTE and tRNS, 13 bytes
     * for each one-byte IDAT, and IEND.
     */
    uint8_t    ihdr[13] = { 0 };
    uint8_t    idat[2u + 5u + sizeof filtered + 4u];
    test_bytes fixture = allocated(8u + 25u + 18u + 18u + sizeof idat * 13u + 12u);
    size_t     at      = 0;

    if (fixture.data == NULL) {
        return fixture;
    }

    /* The signature, IHDR (2 by 1, 8 bits, colour type 2, RGB), the palette and the key. */
    memcpy(fixture.data + at, "\x89PNG\r\n\x1a\n", 8);
    at += 8;
    put_u32be(ihdr, 2);
    put_u32be(ihdr + 4, 1);
    ihdr[8]  = 8;
    ihdr[9]  = 2;
    at      += png_chunk(fixture.data + at, "IHDR", ihdr, sizeof ihdr);
    at      += png_chunk(fixture.data + at, "PLTE", palette, sizeof palette);
    at      += png_chunk(fixture.data + at, "tRNS", key, sizeof key);

    /* The zlib stream: one block of type 0 with the scanlines as they are, then their Adler-32. */
    idat[0] = 0x78;
    idat[1] = 0x01;
    idat[2] = 0x01;
    idat[3] = (uint8_t)sizeof filtered;
    idat[4] = 0;
    idat[5] = (uint8_t)(~(uint32_t)sizeof filtered & 0xffu);
    idat[6] = 0xff;
    memcpy(idat + 7, filtered, sizeof filtered);
    put_u32be(idat + 7u + sizeof filtered, adler32(filtered, sizeof filtered));

    /* Every byte of the zlib stream in an IDAT of its own: one stream, many consecutive chunks. */
    for (size_t part = 0; part < sizeof idat; ++part) {
        at += png_chunk(fixture.data + at, "IDAT", idat + part, 1u);
    }

    at           += png_chunk(fixture.data + at, "IEND", NULL, 0);
    fixture.size  = at;

    return fixture;
}

/**
 * The PNG of fixture_png_rgba with a gAMA chunk of 45455 between IHDR and IDAT: the first 33 bytes
 * (the signature and IHDR), the 16-byte gAMA chunk, then the rest of the file.
 */
test_bytes fixture_png_rgba_gamma(void) {
    test_bytes base    = fixture_png_rgba();
    test_bytes fixture = allocated(base.size + 16u);
    uint8_t    gamma[4];
    size_t     at = 33u;

    if (base.data == NULL || fixture.data == NULL) {
        fixture_free(base);
        fixture_free(fixture);
        return (test_bytes){ NULL, 0 };
    }

    memcpy(fixture.data, base.data, at);
    put_u32be(gamma, 45455u);
    at += png_chunk(fixture.data + at, "gAMA", gamma, sizeof gamma);
    memcpy(fixture.data + at, base.data + 33u, base.size - 33u);
    fixture.size = base.size + 16u;

    fixture_free(base);
    return fixture;
}

/** A 3x2 uncompressed 24-bit TGA whose rows run from the top: six pixels, three bytes each. */
test_bytes fixture_tga_bgrx(void) {
    /*
     * The 18-byte header (type 2, uncompressed true-colour; 3 by 2; 24 bits; the top-left origin
     * bit), then the two rows of pixels, blue first.
     */
    static const uint8_t source[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        3, 0, 2, 0, 24, 0x20,
        30, 20, 10, 60, 50, 40, 90, 80, 70,
        120, 110, 100, 150, 140, 130, 180, 170, 160
    };
    test_bytes fixture = allocated(sizeof source);

    if (fixture.data != NULL) {
        memcpy(fixture.data, source, sizeof source);
    }

    return fixture;
}

/** The 9x5 gradient TGA fixture.h describes, 32 bits a pixel. */
test_bytes fixture_tga_gpu_gradient(void) {
    enum {
        WIDTH  = 9,
        HEIGHT = 5
    };

    uint8_t    bgra[WIDTH * HEIGHT * 4];
    test_bytes fixture = allocated(18u + sizeof bgra);

    if (fixture.data == NULL) {
        return fixture;
    }

    /*
     * Each pixel, blue first: red rises from left to right as blue falls, green rises from top to
     * bottom, and alpha rises from the top-left corner to the bottom-right one.
     */
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            uint8_t      *pixel = bgra + ((size_t)y * WIDTH + x) * 4u;
            const uint8_t red   = (uint8_t)((x * 255u) / (WIDTH - 1u));

            pixel[0] = (uint8_t)(255u - red);
            pixel[1] = (uint8_t)((y * 255u) / (HEIGHT - 1u));
            pixel[2] = red;
            pixel[3] = (uint8_t)(((x + y) * 255u) / (WIDTH + HEIGHT - 2u));
        }
    }

    fixture.size = fixture_tga_build(fixture.data, fixture.size, WIDTH, HEIGHT, 1, bgra);

    return fixture;
}

/** The 16x16 TGA of four flat quadrants fixture.h describes, 32 bits a pixel. */
test_bytes fixture_tga_gpu_flat(void) {
    enum {
        SIDE = 16
    };

    uint8_t    bgra[SIDE * SIDE * 4];
    test_bytes fixture = allocated(18u + sizeof bgra);

    if (fixture.data == NULL) {
        return fixture;
    }

    /*
     * Each pixel takes the colour of its quadrant: 0 at the top left, 1 at the top right, 2 at the
     * bottom left and 3 at the bottom right.
     */
    for (uint32_t y = 0; y < SIDE; ++y) {
        for (uint32_t x = 0; x < SIDE; ++x) {
            uint8_t       *pixel    = bgra + ((size_t)y * SIDE + x) * 4u;
            const uint32_t quadrant = (x < SIDE / 2u ? 0u : 1u) + (y < SIDE / 2u ? 0u : 2u);

            pixel[0] = (uint8_t)(30u + quadrant * 60u);
            pixel[1] = (uint8_t)(200u - quadrant * 50u);
            pixel[2] = (uint8_t)(80u + quadrant * 40u);
            pixel[3] = (uint8_t)(255u - quadrant * 30u);
        }
    }

    fixture.size = fixture_tga_build(fixture.data, fixture.size, SIDE, SIDE, 1, bgra);

    return fixture;
}

/** The size of an uncompressed TGA: the 18-byte header and three or four bytes a pixel. */
size_t fixture_tga_bytes(uint32_t width, uint32_t height, int with_alpha) {
    return 18u + (size_t)width * height * (with_alpha ? 4u : 3u);
}

/**
 * Writes the TGA of `width` by `height` BGRA samples into `output`, 32 bits a pixel with alpha or
 * 24 without it. Returns the bytes written, or 0 when `output` is NULL or `capacity` too small.
 */
size_t fixture_tga_build(
    uint8_t       *output,
    size_t         capacity,
    uint32_t       width,
    uint32_t       height,
    int            with_alpha,
    const uint8_t *bgra) {
    const size_t total = fixture_tga_bytes(width, height, with_alpha);

    if (output == NULL || capacity < total) {
        return 0;
    }

    /*
     * The header: type 2, uncompressed true-colour; the width and the height, low byte first; the
     * bits per pixel.
     */
    memset(output, 0, 18);
    output[2]  = 2;
    output[12] = (uint8_t)(width & 0xffu);
    output[13] = (uint8_t)(width >> 8);
    output[14] = (uint8_t)(height & 0xffu);
    output[15] = (uint8_t)(height >> 8);
    output[16] = with_alpha ? 32u : 24u;
    /* Bit five is a top-left origin; the low nibble counts the attribute bits alpha occupies. */
    output[17] = (uint8_t)(0x20u | (with_alpha ? 8u : 0u));

    /* The pixels from the top row down: blue, green and red, then alpha when it is kept. */
    for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
        output[18u + pixel * (with_alpha ? 4u : 3u)] = bgra[pixel * 4u];
        output[19u + pixel * (with_alpha ? 4u : 3u)] = bgra[pixel * 4u + 1u];
        output[20u + pixel * (with_alpha ? 4u : 3u)] = bgra[pixel * 4u + 2u];

        if (with_alpha) {
            output[21u + pixel * 4u] = bgra[pixel * 4u + 3u];
        }
    }

    return total;
}

/**
 * Where the values after a directory of `tag_count` entries begin: past the 8-byte header, the
 * 2-byte entry count, 12 bytes per entry, and the 4-byte offset of a next directory.
 */
size_t fixture_tiff_ifd_end(size_t tag_count) {
    return 8u + 2u + tag_count * 12u + 4u;
}

/**
 * Writes a TIFF of one directory into `output`, in either byte order: the header, the directory
 * with `tags` as its entries, then the `trailing` values the entries point to. Returns the bytes
 * written, or 0 when `output` is NULL or `capacity` too small.
 */
size_t fixture_tiff_build(
    uint8_t                *output,
    size_t                  capacity,
    int                     big_endian,
    const fixture_tiff_tag *tags,
    size_t                  tag_count,
    const uint8_t          *trailing,
    size_t                  trailing_size) {
    const size_t ifd_end = fixture_tiff_ifd_end(tag_count);
    const size_t total   = ifd_end + trailing_size;
    size_t       at;

    if (output == NULL || capacity < total) {
        return 0;
    }

    /*
     * Everything zeroed, then the header: MM for big-endian or II for little-endian, and 42 in
     * that byte order.
     */
    memset(output, 0, total);
    memcpy(output, big_endian ? "MM\0\x2a" : "II\x2a\0", 4);

    /* The directory's offset, 8, right after the header, and its entry count. */
    if (big_endian) {
        put_u32be(output + 4, 8);
        output[8] = (uint8_t)(tag_count >> 8);
        output[9] = (uint8_t)tag_count;
    } else {
        put_u32(output + 4, 8);
        output[8] = (uint8_t)tag_count;
        output[9] = (uint8_t)(tag_count >> 8);
    }

    /* The entries, 12 bytes each: the tag, the type, the count and the value. */
    for (at = 0; at < tag_count; ++at) {
        uint8_t *entry = output + 10u + at * 12u;

        /* The size of one value: a byte for types 1 and 2, two bytes for type 3, four otherwise. */
        const uint32_t element = tags[at].type == 1u || tags[at].type == 2u ? 1u : tags[at].type == 3u ? 2u
                                                                                                       : 4u;

        const int inline_short = tags[at].count == 1u && element == 2u;

        if (big_endian) {
            entry[0] = (uint8_t)(tags[at].tag >> 8);
            entry[1] = (uint8_t)tags[at].tag;
            entry[2] = (uint8_t)(tags[at].type >> 8);
            entry[3] = (uint8_t)tags[at].type;
            put_u32be(entry + 4, tags[at].count);

            /* A single 2-byte value goes in the first two bytes of the value field. */
            if (inline_short) {
                entry[8] = (uint8_t)(tags[at].value >> 8);
                entry[9] = (uint8_t)tags[at].value;
            } else {
                put_u32be(entry + 8, tags[at].value);
            }
        } else {
            entry[0] = (uint8_t)tags[at].tag;
            entry[1] = (uint8_t)(tags[at].tag >> 8);
            entry[2] = (uint8_t)tags[at].type;
            entry[3] = (uint8_t)(tags[at].type >> 8);
            put_u32(entry + 4, tags[at].count);
            put_u32(entry + 8, tags[at].value);
        }
    }

    /* The values the entries point to, right after the directory. */
    if (trailing_size != 0) {
        memcpy(output + ifd_end, trailing, trailing_size);
    }

    return total;
}

/**
 * Writes the JPEG `spec` describes into `output`, one segment after another: the start of image, an
 * APP1 when the spec has EXIF bytes, the quantisation table, the frame header, the two Huffman
 * tables, a restart interval when the spec asks for one, the scan header, the entropy-coded data
 * and the end of image. Returns the bytes written, or 0 when `output` is NULL, the component count
 * is not 1 to 3, or `capacity` is less than 256 bytes beyond the EXIF and entropy data.
 */
size_t fixture_jpeg_build(uint8_t *output, size_t capacity, const fixture_jpeg_spec *spec) {
    /*
     * Every quantisation entry is one, the DC Huffman table holds three symbols and the AC table
     * only 0x00, the end of block, so a DC-only block decodes to a flat sample a test can predict
     * exactly: coefficient / 8 + 128.
     */
    static const uint8_t quantisation[65] = { 0 };
    static const uint8_t dc_table[20]     = {
        0x00, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x09, 0x0a
    };
    static const uint8_t ac_table[18] = {
        0x10, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00
    };

    const uint8_t components = spec->component_count;
    size_t        at         = 0;

    if (output == NULL || components == 0u || components > 3u) {
        return 0;
    }

    if (capacity < 256u + spec->exif_size + spec->entropy_size) {
        return 0;
    }

    /* The start of image, then the EXIF bytes in an APP1 segment when there are any. */
    output[at++] = 0xff;
    output[at++] = 0xd8;

    if (spec->exif_size != 0) {
        output[at++] = 0xff;
        output[at++] = 0xe1;
        put_u16be(output + at, (uint16_t)(spec->exif_size + 2u));
        at += 2;
        memcpy(output + at, spec->exif, spec->exif_size);
        at += spec->exif_size;
    }

    /* The quantisation table: its length, 67, then table 0 with all 64 entries set to 1. */
    output[at++] = 0xff;
    output[at++] = 0xdb;
    put_u16be(output + at, 67);
    at += 2;
    memcpy(output + at, quantisation, sizeof quantisation);

    for (size_t entry = 1; entry <= 64u; ++entry) {
        output[at + entry] = 1;
    }

    at += sizeof quantisation;

    /*
     * The frame header: its length, the precision, the height, the width and the component count,
     * then for each component its number, its sampling factors and quantisation table 0.
     */
    output[at++] = 0xff;
    output[at++] = spec->frame_marker;
    put_u16be(output + at, (uint16_t)(8u + (size_t)components * 3u));
    at           += 2;
    output[at++]  = spec->precision;
    put_u16be(output + at, spec->height);
    at += 2;
    put_u16be(output + at, spec->width);
    at           += 2;
    output[at++]  = components;

    for (uint8_t component = 0; component < components; ++component) {
        output[at++] = (uint8_t)(component + 1u);
        output[at++] = component == 0u ? spec->luma_sampling : 0x11u;
        output[at++] = 0x00;
    }

    /* The Huffman tables, each in a segment of its own: the DC table, then the AC table. */
    output[at++] = 0xff;
    output[at++] = 0xc4;
    put_u16be(output + at, (uint16_t)(sizeof dc_table + 2u));
    at += 2;
    memcpy(output + at, dc_table, sizeof dc_table);
    at += sizeof dc_table;

    output[at++] = 0xff;
    output[at++] = 0xc4;
    put_u16be(output + at, (uint16_t)(sizeof ac_table + 2u));
    at += 2;
    memcpy(output + at, ac_table, sizeof ac_table);
    at += sizeof ac_table;

    /* The restart interval segment, when the spec asks for one: its length, 4, and the interval. */
    if (spec->restart_interval != 0u) {
        output[at++] = 0xff;
        output[at++] = 0xdd;
        put_u16be(output + at, 4);
        at += 2;
        put_u16be(output + at, spec->restart_interval);
        at += 2;
    }

    /*
     * The scan header: its length and the component count, each component's number with Huffman
     * tables 0, then a full spectral selection, 0 to 63, with no successive approximation.
     */
    output[at++] = 0xff;
    output[at++] = 0xda;
    put_u16be(output + at, (uint16_t)(6u + (size_t)components * 2u));
    at           += 2;
    output[at++]  = components;

    for (uint8_t component = 0; component < components; ++component) {
        output[at++] = (uint8_t)(component + 1u);
        output[at++] = 0x00;
    }

    output[at++] = 0x00;
    output[at++] = 0x3f;
    output[at++] = 0x00;

    /* The entropy-coded data, and the end of image unless the spec leaves it off. */
    memcpy(output + at, spec->entropy, spec->entropy_size);
    at += spec->entropy_size;

    if (!spec->omit_end_of_image) {
        output[at++] = 0xff;
        output[at++] = 0xd9;
    }

    return at;
}

/** 16x8 in two flat DC-only MCUs: grey 78 on the left, grey 178 on the right. */
test_bytes fixture_jpeg_ycbcr(void) {
    /* The entropy-coded data of the two MCUs. */
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xf2, 0x00, 0x7f };

    fixture_jpeg_spec spec = {
        .frame_marker    = 0xc0,
        .precision       = 8,
        .width           = 16,
        .height          = 8,
        .component_count = 3,
        .luma_sampling   = 0x11,
        .entropy         = entropy,
        .entropy_size    = sizeof entropy
    };

    test_bytes fixture = allocated(512);

    if (fixture.data == NULL) {
        return fixture;
    }

    fixture.size = fixture_jpeg_build(fixture.data, 512, &spec);

    if (fixture.size == 0) {
        fixture_free(fixture);
        return (test_bytes){ NULL, 0 };
    }

    return fixture;
}

/** 3x2 uncompressed RGB, the same pixels the TGA fixture carries, so both agree on the result. */
test_bytes fixture_tiff_rgb(void) {
    static const uint8_t pixels[] = {
        10, 20, 30, 40, 50, 60, 70, 80, 90,
        100, 110, 120, 130, 140, 150, 160, 170, 180
    };

    /* Where the values after the directory go: the bits per sample (three), then the pixels. */
    const uint32_t bits_at   = (uint32_t)fixture_tiff_ifd_end(9);
    const uint32_t pixels_at = bits_at + 6u;

    /*
     * The directory: width 3, height 2, bits per sample (three values, stored at bits_at), no
     * compression, photometric 2 (RGB), the strip at pixels_at, three samples per pixel, two rows
     * per strip, and the strip's byte count.
     */
    const fixture_tiff_tag tags[9] = {
        { 256, 3, 1, 3 },
        { 257, 3, 1, 2 },
        { 258, 3, 3, bits_at },
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 1, pixels_at },
        { 277, 3, 1, 3 },
        { 278, 3, 1, 2 },
        { 279, 4, 1, (uint32_t)sizeof pixels }
    };

    uint8_t    trailing[6 + sizeof pixels];
    test_bytes fixture = allocated(pixels_at + sizeof pixels);

    /* The values after the directory: 8 bits for each of the three samples, then the pixels. */
    memset(trailing, 0, sizeof trailing);
    trailing[0] = 8;
    trailing[2] = 8;
    trailing[4] = 8;
    memcpy(trailing + 6, pixels, sizeof pixels);

    if (fixture.data == NULL) {
        return fixture;
    }

    fixture.size = fixture_tiff_build(fixture.data, fixture.size, 0, tags, 9, trailing, sizeof trailing);

    if (fixture.size == 0) {
        fixture_free(fixture);
        return (test_bytes){ NULL, 0 };
    }

    return fixture;
}

/** Releases a fixture's bytes; an empty fixture is fine. */
void fixture_free(test_bytes fixture) {
    free(fixture.data);
}

/**
 * Writes a fixture's bytes to the file at `path`, replacing what was there. Returns 1 when every
 * byte was written and the file closed cleanly, 0 otherwise.
 */
int fixture_write(const char *path, test_bytes fixture) {
    FILE  *file = fopen(path, "wb");
    size_t written;

    if (file == NULL) {
        return 0;
    }

    written = fwrite(fixture.data, 1, fixture.size, file);

    return fclose(file) == 0 && written == fixture.size;
}
