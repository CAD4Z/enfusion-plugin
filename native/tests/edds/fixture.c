#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DDS_HEADER_BYTES = 128
};

static void put_u32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static void put_u32be(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

static void put_u16be(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

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

static uint32_t adler32(const uint8_t *bytes, size_t size) {
    uint32_t first  = 1;
    uint32_t second = 0;
    for (size_t at = 0; at < size; ++at) {
        first  = (first + bytes[at]) % 65521u;
        second = (second + first) % 65521u;
    }
    return (second << 16) | first;
}

static size_t png_chunk(uint8_t *output, const char type[4], const uint8_t *data, uint32_t size) {
    put_u32be(output, size);
    memcpy(output + 4, type, 4);
    if (size != 0) {
        memcpy(output + 8, data, size);
    }
    put_u32be(output + 8u + size, crc32(output + 4, 4u + size));
    return 12u + size;
}

static void header(
    uint8_t   *bytes,
    uint32_t   width,
    uint32_t   height,
    uint32_t   mips,
    const char four_cc[4],
    uint32_t   alpha_mask) {
    memset(bytes, 0, DDS_HEADER_BYTES);
    memcpy(bytes, "DDS ", 4);
    put_u32(bytes + 4, 124);
    put_u32(bytes + 8, 0x0002100fu);
    put_u32(bytes + 12, height);
    put_u32(bytes + 16, width);
    put_u32(bytes + 20, width * 4u);
    put_u32(bytes + 28, mips);
    memcpy(bytes + 36, "ENF1", 4);
    put_u32(bytes + 76, 32);
    put_u32(bytes + 80, four_cc[0] == '\0' ? (alpha_mask == 0 ? 0x40u : 0x41u) : 0x04u);
    memcpy(bytes + 84, four_cc, 4);
    put_u32(bytes + 88, four_cc[0] == '\0' ? 32u : 0u);
    put_u32(bytes + 92, four_cc[0] == '\0' ? 0x00ff0000u : 0u);
    put_u32(bytes + 96, four_cc[0] == '\0' ? 0x0000ff00u : 0u);
    put_u32(bytes + 100, four_cc[0] == '\0' ? 0x000000ffu : 0u);
    put_u32(bytes + 104, alpha_mask);
    put_u32(bytes + 108, mips > 1u ? 0x00401008u : 0x00001000u);
}

static test_bytes allocated(size_t size) {
    test_bytes fixture = { calloc(1, size), size };
    if (fixture.data == NULL) {
        fixture.size = 0;
    }
    return fixture;
}

test_bytes fixture_copy_bgra(void) {
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
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof small);
    memcpy(fixture.data + 136, "COPY", 4);
    put_u32(fixture.data + 140, (uint32_t)sizeof large);
    memcpy(fixture.data + 144, small, sizeof small);
    memcpy(fixture.data + 148, large, sizeof large);
    return fixture;
}

static size_t literal_block(uint8_t *output, const uint8_t *raw, size_t length) {
    size_t at        = 0;
    size_t remaining = length > 15u ? length - 15u : 0u;
    output[at++]     = (uint8_t)((length < 15u ? length : 15u) << 4);
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

test_bytes fixture_lz4_bgrx(void) {
    static const uint8_t raw[] = { 3, 2, 1, 0, 6, 5, 4, 17 };
    uint8_t              compressed[32];
    const size_t         compressed_size = literal_block(compressed, raw, sizeof raw);
    const uint32_t       stored          = (uint32_t)(4u + 4u + compressed_size);
    test_bytes           fixture         = allocated(DDS_HEADER_BYTES + 8u + stored);
    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 2, 1, 1, "\0\0\0\0", 0u);
    memcpy(fixture.data + 128, "LZ4 ", 4);
    put_u32(fixture.data + 132, stored);
    put_u32(fixture.data + 136, (uint32_t)sizeof raw);
    put_u32(fixture.data + 140, 0x80000000u | (uint32_t)compressed_size);
    memcpy(fixture.data + 144, compressed, compressed_size);
    return fixture;
}

test_bytes fixture_lz4_streaming_bgrx(void) {
    enum {
        RAW_BYTES   = 256 * 65 * 4,
        FIRST_BYTES = 65536
    };

    uint8_t             *raw   = malloc(RAW_BYTES);
    uint8_t             *first = malloc(FIRST_BYTES + 300u);
    size_t               first_size;
    static const uint8_t second[] = { 0x0f, 0x04, 0x00, 0xff, 0xff, 0xff, 0xf0 };
    test_bytes           fixture;
    size_t               payload;
    size_t               at;

    if (raw == NULL || first == NULL) {
        free(raw);
        free(first);
        fixture.data = NULL;
        fixture.size = 0;
        return fixture;
    }
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

test_bytes fixture_dx10_bgrx(void) {
    static const uint8_t raw[]   = { 30, 20, 10, 77, 60, 50, 40, 88 };
    test_bytes           fixture = allocated(DDS_HEADER_BYTES + 20u + 8u + sizeof raw);
    if (fixture.data == NULL) {
        return fixture;
    }
    header(fixture.data, 2, 1, 1, "DX10", 0u);
    put_u32(fixture.data + 128, 93u);
    put_u32(fixture.data + 132, 3u);
    put_u32(fixture.data + 136, 0u);
    put_u32(fixture.data + 140, 1u);
    put_u32(fixture.data + 144, 0u);
    memcpy(fixture.data + 148, "COPY", 4);
    put_u32(fixture.data + 152, (uint32_t)sizeof raw);
    memcpy(fixture.data + 156, raw, sizeof raw);
    return fixture;
}

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

test_bytes fixture_dds_bgrx_mips(void) {
    static const uint8_t top[] = {
        3, 2, 1, 0, 6, 5, 4, 0, 9, 8, 7, 0, 12, 11, 10, 0,
        15, 14, 13, 0, 18, 17, 16, 0, 21, 20, 19, 0, 24, 23, 22, 0
    };
    static const uint8_t middle[] = { 33, 22, 11, 0, 66, 55, 44, 0 };
    static const uint8_t last[]   = { 99, 88, 77, 0 };
    test_bytes           fixture  = allocated(DDS_HEADER_BYTES + sizeof top + sizeof middle + sizeof last);
    size_t               at       = DDS_HEADER_BYTES;
    if (fixture.data == NULL) {
        return fixture;
    }
    header(fixture.data, 4, 2, 3, "\0\0\0\0", 0u);
    memset(fixture.data + 32, 0, 44);
    put_u32(fixture.data + 76, 32);
    put_u32(fixture.data + 80, 0x40u);
    put_u32(fixture.data + 88, 32u);
    put_u32(fixture.data + 92, 0x00ff0000u);
    put_u32(fixture.data + 96, 0x0000ff00u);
    put_u32(fixture.data + 100, 0x000000ffu);
    memcpy(fixture.data + at, top, sizeof top);
    at += sizeof top;
    memcpy(fixture.data + at, middle, sizeof middle);
    at += sizeof middle;
    memcpy(fixture.data + at, last, sizeof last);
    return fixture;
}

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

test_bytes fixture_dds_bgra_alpha_mips(void) {
    static const uint8_t top[] = {
        3, 2, 1, 255, 6, 5, 4, 255, 9, 8, 7, 255, 12, 11, 10, 255,
        15, 14, 13, 255, 18, 17, 16, 255, 21, 20, 19, 255, 24, 23, 22, 255
    };
    static const uint8_t middle[] = { 33, 22, 11, 128, 66, 55, 44, 128 };
    static const uint8_t last[]   = { 99, 88, 77, 64 };
    test_bytes           fixture  = allocated(DDS_HEADER_BYTES + sizeof top + sizeof middle + sizeof last);
    size_t               at       = DDS_HEADER_BYTES;
    if (fixture.data == NULL) {
        return fixture;
    }
    header(fixture.data, 4, 2, 3, "\0\0\0\0", 0xff000000u);
    memset(fixture.data + 32, 0, 44);
    memcpy(fixture.data + at, top, sizeof top);
    at += sizeof top;
    memcpy(fixture.data + at, middle, sizeof middle);
    at += sizeof middle;
    memcpy(fixture.data + at, last, sizeof last);
    return fixture;
}

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
    header(fixture.data, 4, 4, 1, "DX10", 0u);
    put_u32(fixture.data + 8, compressed ? 0x00081007u : 0x0000100fu);
    put_u32(fixture.data + 20, compressed ? (uint32_t)payload_size : 4u * bytes_per_pixel);
    memset(fixture.data + 32, 0, 44);
    put_u32(fixture.data + 128, dxgi_format);
    put_u32(fixture.data + 132, 3u);
    put_u32(fixture.data + 140, 1u);
    memcpy(fixture.data + DDS_HEADER_BYTES + 20u, payload, payload_size);
    return fixture;
}

test_bytes fixture_dds_dx10_r8_mips(void) {
    static const uint8_t top[]    = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint8_t middle[] = { 11, 22 };
    static const uint8_t last[]   = { 33 };
    test_bytes           fixture  = allocated(DDS_HEADER_BYTES + 20u +
                   sizeof top + sizeof middle + sizeof last);
    size_t               at       = DDS_HEADER_BYTES + 20u;
    if (fixture.data == NULL) {
        return fixture;
    }
    header(fixture.data, 4, 2, 3, "DX10", 0u);
    put_u32(fixture.data + 20, 4u);
    memset(fixture.data + 32, 0, 44);
    put_u32(fixture.data + 128, 61u);
    put_u32(fixture.data + 132, 3u);
    put_u32(fixture.data + 140, 1u);
    memcpy(fixture.data + at, top, sizeof top);
    at += sizeof top;
    memcpy(fixture.data + at, middle, sizeof middle);
    at += sizeof middle;
    memcpy(fixture.data + at, last, sizeof last);
    return fixture;
}

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

test_bytes fixture_integer_overflow(void) {
    test_bytes fixture = fixture_copy_bgra();
    if (fixture.data != NULL) {
        memset(fixture.data + 12, 0xff, 8);
    }
    return fixture;
}

/* Owned fixed-Huffman Deflate: one zero, then distance-one runs, without a codec dependency. */
static void fixture_bits(uint8_t *bytes, size_t *bit, unsigned value, unsigned count) {
    for (unsigned at = 0; at < count; ++at, ++*bit) {
        bytes[*bit / 8u] |= (uint8_t)(((value >> at) & 1u) << (*bit % 8u));
    }
}

test_bytes fixture_png_flat(uint32_t side) {
    const uint32_t filtered = (side * 4u + 1u) * side;
    const size_t   capacity = (size_t)filtered / 258u * 2u + 2048u;
    test_bytes     fixture;
    uint8_t       *idat;
    uint8_t        ihdr[13]  = { 0 };
    size_t         bit       = 16u;
    size_t         at        = 8u;
    uint32_t       remaining = filtered - 1u;
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
    idat[0] = 0x78;
    idat[1] = 0x01;
    fixture_bits(idat, &bit, 3u, 3u);  /* Final block, fixed Huffman. */
    fixture_bits(idat, &bit, 12u, 8u); /* Literal zero. */
    while (remaining >= 258u) {
        fixture_bits(idat, &bit, 163u, 8u); /* Length 258. */
        fixture_bits(idat, &bit, 0u, 5u);   /* Distance one. */
        remaining -= 258u;
    }
    while (remaining-- > 0u) {
        fixture_bits(idat, &bit, 12u, 8u);
    }
    fixture_bits(idat, &bit, 0u, 7u); /* End of block. */
    put_u32be(idat + (bit + 7u) / 8u, ((filtered % 65521u) << 16) | 1u);
    memcpy(fixture.data, "\x89PNG\r\n\x1a\n", 8u);
    put_u32be(ihdr, side);
    put_u32be(ihdr + 4u, side);
    ihdr[8]       = 8u;
    ihdr[9]       = 6u;
    at           += png_chunk(fixture.data + at, "IHDR", ihdr, sizeof ihdr);
    at           += png_chunk(fixture.data + at, "IDAT", idat, (uint32_t)((bit + 7u) / 8u + 4u));
    at           += png_chunk(fixture.data + at, "IEND", NULL, 0u);
    fixture.size  = at;
    free(idat);
    return fixture;
}

test_bytes fixture_png_rgba(void) {
    static const uint8_t filtered[] = {
        0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
        0, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220
    };
    uint8_t    ihdr[13] = { 0 };
    uint8_t    idat[2u + 5u + sizeof filtered + 4u];
    test_bytes fixture = allocated(8u + 25u + 12u + sizeof idat + 12u);
    size_t     at      = 0;
    if (fixture.data == NULL) {
        return fixture;
    }
    memcpy(fixture.data + at, "\x89PNG\r\n\x1a\n", 8);
    at += 8;
    put_u32be(ihdr, 3);
    put_u32be(ihdr + 4, 2);
    ihdr[8]  = 8;
    ihdr[9]  = 6;
    at      += png_chunk(fixture.data + at, "IHDR", ihdr, sizeof ihdr);
    idat[0]  = 0x78;
    idat[1]  = 0x01;
    idat[2]  = 0x01;
    idat[3]  = (uint8_t)sizeof filtered;
    idat[4]  = 0;
    idat[5]  = (uint8_t)(~(uint32_t)sizeof filtered & 0xffu);
    idat[6]  = 0xff;
    memcpy(idat + 7, filtered, sizeof filtered);
    put_u32be(idat + 7u + sizeof filtered, adler32(filtered, sizeof filtered));
    at           += png_chunk(fixture.data + at, "IDAT", idat, sizeof idat);
    at           += png_chunk(fixture.data + at, "IEND", NULL, 0);
    fixture.size  = at;
    return fixture;
}

test_bytes fixture_png_rgb_keyed(void) {
    /* Two RGB pixels, (10,20,30) and (40,50,60); the first is the transparent key. */
    static const uint8_t filtered[] = { 0, 10, 20, 30, 40, 50, 60 };
    static const uint8_t palette[]  = { 1, 2, 3, 4, 5, 6 };
    static const uint8_t key[]      = { 0, 10, 0, 20, 0, 30 };
    uint8_t              ihdr[13]   = { 0 };
    uint8_t              idat[2u + 5u + sizeof filtered + 4u];
    /* Every byte of the zlib stream in an IDAT of its own: one stream, many consecutive chunks. */
    test_bytes           fixture = allocated(8u + 25u + 18u + 18u + sizeof idat * 13u + 12u);
    size_t               at      = 0;
    if (fixture.data == NULL) {
        return fixture;
    }
    memcpy(fixture.data + at, "\x89PNG\r\n\x1a\n", 8);
    at += 8;
    put_u32be(ihdr, 2);
    put_u32be(ihdr + 4, 1);
    ihdr[8]  = 8;
    ihdr[9]  = 2;
    at      += png_chunk(fixture.data + at, "IHDR", ihdr, sizeof ihdr);
    at      += png_chunk(fixture.data + at, "PLTE", palette, sizeof palette);
    at      += png_chunk(fixture.data + at, "tRNS", key, sizeof key);
    idat[0]  = 0x78;
    idat[1]  = 0x01;
    idat[2]  = 0x01;
    idat[3]  = (uint8_t)sizeof filtered;
    idat[4]  = 0;
    idat[5]  = (uint8_t)(~(uint32_t)sizeof filtered & 0xffu);
    idat[6]  = 0xff;
    memcpy(idat + 7, filtered, sizeof filtered);
    put_u32be(idat + 7u + sizeof filtered, adler32(filtered, sizeof filtered));
    for (size_t part = 0; part < sizeof idat; ++part) {
        at += png_chunk(fixture.data + at, "IDAT", idat + part, 1u);
    }
    at           += png_chunk(fixture.data + at, "IEND", NULL, 0);
    fixture.size  = at;
    return fixture;
}

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

test_bytes fixture_tga_bgrx(void) {
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
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            uint8_t      *pixel = bgra + ((size_t)y * WIDTH + x) * 4u;
            const uint8_t red   = (uint8_t)((x * 255u) / (WIDTH - 1u));
            pixel[0]            = (uint8_t)(255u - red);
            pixel[1]            = (uint8_t)((y * 255u) / (HEIGHT - 1u));
            pixel[2]            = red;
            pixel[3]            = (uint8_t)(((x + y) * 255u) / (WIDTH + HEIGHT - 2u));
        }
    }
    fixture.size = fixture_tga_build(fixture.data, fixture.size, WIDTH, HEIGHT, 1, bgra);
    return fixture;
}

test_bytes fixture_tga_gpu_flat(void) {
    enum {
        SIDE = 16
    };

    uint8_t    bgra[SIDE * SIDE * 4];
    test_bytes fixture = allocated(18u + sizeof bgra);
    if (fixture.data == NULL) {
        return fixture;
    }
    for (uint32_t y = 0; y < SIDE; ++y) {
        for (uint32_t x = 0; x < SIDE; ++x) {
            uint8_t       *pixel    = bgra + ((size_t)y * SIDE + x) * 4u;
            const uint32_t quadrant = (x < SIDE / 2u ? 0u : 1u) + (y < SIDE / 2u ? 0u : 2u);
            pixel[0]                = (uint8_t)(30u + quadrant * 60u);
            pixel[1]                = (uint8_t)(200u - quadrant * 50u);
            pixel[2]                = (uint8_t)(80u + quadrant * 40u);
            pixel[3]                = (uint8_t)(255u - quadrant * 30u);
        }
    }
    fixture.size = fixture_tga_build(fixture.data, fixture.size, SIDE, SIDE, 1, bgra);
    return fixture;
}

size_t fixture_tga_bytes(uint32_t width, uint32_t height, int with_alpha) {
    return 18u + (size_t)width * height * (with_alpha ? 4u : 3u);
}

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
    memset(output, 0, 18);
    output[2]  = 2;
    output[12] = (uint8_t)(width & 0xffu);
    output[13] = (uint8_t)(width >> 8);
    output[14] = (uint8_t)(height & 0xffu);
    output[15] = (uint8_t)(height >> 8);
    output[16] = with_alpha ? 32u : 24u;
    /* Bit five is a top-left origin; the low nibble counts the attribute bits alpha occupies. */
    output[17] = (uint8_t)(0x20u | (with_alpha ? 8u : 0u));
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

size_t fixture_tiff_ifd_end(size_t tag_count) {
    return 8u + 2u + tag_count * 12u + 4u;
}

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
    memset(output, 0, total);
    memcpy(output, big_endian ? "MM\0\x2a" : "II\x2a\0", 4);
    if (big_endian) {
        put_u32be(output + 4, 8);
        output[8] = (uint8_t)(tag_count >> 8);
        output[9] = (uint8_t)tag_count;
    } else {
        put_u32(output + 4, 8);
        output[8] = (uint8_t)tag_count;
        output[9] = (uint8_t)(tag_count >> 8);
    }
    for (at = 0; at < tag_count; ++at) {
        uint8_t       *entry        = output + 10u + at * 12u;
        const uint32_t element      = tags[at].type == 1u || tags[at].type == 2u ? 1u
                 : tags[at].type == 3u                                           ? 2u
                                                                                 : 4u;
        const int      inline_short = tags[at].count == 1u && element == 2u;
        if (big_endian) {
            entry[0] = (uint8_t)(tags[at].tag >> 8);
            entry[1] = (uint8_t)tags[at].tag;
            entry[2] = (uint8_t)(tags[at].type >> 8);
            entry[3] = (uint8_t)tags[at].type;
            put_u32be(entry + 4, tags[at].count);
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
    if (trailing_size != 0) {
        memcpy(output + ifd_end, trailing, trailing_size);
    }
    return total;
}

size_t fixture_jpeg_build(uint8_t *output, size_t capacity, const fixture_jpeg_spec *spec) {
    /*
     * Every quantisation entry is one and both Huffman tables are three symbols wide, so a DC-only
     * block decodes to a flat sample a test can predict exactly: coefficient / 8 + 128.
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
    output[at++] = 0xff;
    output[at++] = 0xdb;
    put_u16be(output + at, 67);
    at += 2;
    memcpy(output + at, quantisation, sizeof quantisation);
    for (size_t entry = 1; entry <= 64u; ++entry) {
        output[at + entry] = 1;
    }
    at           += sizeof quantisation;
    output[at++]  = 0xff;
    output[at++]  = spec->frame_marker;
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
    output[at++] = 0xff;
    output[at++] = 0xc4;
    put_u16be(output + at, (uint16_t)(sizeof dc_table + 2u));
    at += 2;
    memcpy(output + at, dc_table, sizeof dc_table);
    at           += sizeof dc_table;
    output[at++]  = 0xff;
    output[at++]  = 0xc4;
    put_u16be(output + at, (uint16_t)(sizeof ac_table + 2u));
    at += 2;
    memcpy(output + at, ac_table, sizeof ac_table);
    at += sizeof ac_table;
    if (spec->restart_interval != 0u) {
        output[at++] = 0xff;
        output[at++] = 0xdd;
        put_u16be(output + at, 4);
        at += 2;
        put_u16be(output + at, spec->restart_interval);
        at += 2;
    }
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
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xf2, 0x00, 0x7f };
    fixture_jpeg_spec    spec      = { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 8, .component_count = 3, .luma_sampling = 0x11, .entropy = entropy, .entropy_size = sizeof entropy };
    test_bytes           fixture   = allocated(512);
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
    const uint32_t         bits_at   = (uint32_t)fixture_tiff_ifd_end(9);
    const uint32_t         pixels_at = bits_at + 6u;
    const fixture_tiff_tag tags[9]   = {
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

void fixture_free(test_bytes fixture) {
    free(fixture.data);
}

int fixture_write(const char *path, test_bytes fixture) {
    FILE  *file = fopen(path, "wb");
    size_t written;
    if (file == NULL) {
        return 0;
    }
    written = fwrite(fixture.data, 1, fixture.size, file);
    return fclose(file) == 0 && written == fixture.size;
}
