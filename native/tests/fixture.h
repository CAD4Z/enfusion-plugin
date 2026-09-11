#ifndef EDDS_TEST_FIXTURE_H
#define EDDS_TEST_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct test_bytes {
    uint8_t *data;
    size_t size;
} test_bytes;

test_bytes fixture_copy_bgra(void);
test_bytes fixture_lz4_bgrx(void);
test_bytes fixture_lz4_streaming_bgrx(void);
test_bytes fixture_dx10_bgrx(void);
test_bytes fixture_dxt1(void);
test_bytes fixture_odd_fourcc(void);
test_bytes fixture_integer_overflow(void);
test_bytes fixture_png_rgba(void);
test_bytes fixture_png_rgba_gamma(void);
test_bytes fixture_tga_bgrx(void);
test_bytes fixture_jpeg_ycbcr(void);
test_bytes fixture_tiff_rgb(void);
void fixture_free(test_bytes fixture);
int fixture_write(const char *path, test_bytes fixture);

/**
 * One IFD entry as the builder takes it. `value` is the literal inline value for a tag that fits
 * four bytes, and the file offset of the values otherwise — `fixture_tiff_ifd_end` says where the
 * trailing blob a test supplies begins.
 */
typedef struct fixture_tiff_tag {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t value;
} fixture_tiff_tag;

size_t fixture_tiff_ifd_end(size_t tag_count);
size_t fixture_tiff_build(
    uint8_t *output,
    size_t capacity,
    int big_endian,
    const fixture_tiff_tag *tags,
    size_t tag_count,
    const uint8_t *trailing,
    size_t trailing_size
);

/**
 * A JPEG around fixed all-ones quantisation and a three-symbol Huffman pair, so a test writes only
 * the frame shape it is about and the entropy bits it wants decoded.
 */
typedef struct fixture_jpeg_spec {
    uint8_t frame_marker;
    uint8_t precision;
    uint16_t width;
    uint16_t height;
    uint8_t component_count;
    uint8_t luma_sampling;
    uint16_t restart_interval;
    const uint8_t *exif;
    size_t exif_size;
    const uint8_t *entropy;
    size_t entropy_size;
    int omit_end_of_image;
} fixture_jpeg_spec;

size_t fixture_jpeg_build(uint8_t *output, size_t capacity, const fixture_jpeg_spec *spec);

#endif
