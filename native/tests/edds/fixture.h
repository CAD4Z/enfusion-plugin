#ifndef EDDS_TEST_FIXTURE_H
#define EDDS_TEST_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct test_bytes {
    uint8_t *data;
    size_t   size;
} test_bytes;

test_bytes fixture_copy_bgra(void);
test_bytes fixture_lz4_bgrx(void);
test_bytes fixture_lz4_streaming_bgrx(void);
test_bytes fixture_dx10_bgrx(void);
test_bytes fixture_dxt1(void);
test_bytes fixture_odd_fourcc(void);

/**
 * A DXT5 whose colour endpoints are in the order BC1 would read as its punch-through layout. BC3
 * has no such layout — the alpha block beside it carries the alpha — so this is a legal texture
 * whose fourth colour a BC1 decoder would turn into a hole.
 */
test_bytes fixture_dxt5_low_endpoints(void);
/** A standard DDS, not ENF1, whose three levels carry independently chosen pixels. */
test_bytes fixture_dds_bgrx_mips(void);
/** The same standard DDS with only its top level, suitable for generated-mip coverage. */
test_bytes fixture_dds_bgrx_top(void);
/** Controlled compressed legacy and DX10 source-header paths. */
test_bytes fixture_dds_dxt1_top(void);
test_bytes fixture_dds_dxt5_top(void);
test_bytes fixture_dds_bgra_alpha_mips(void);
test_bytes fixture_dds_dx10_r8_mips(void);
/** A controlled four-by-four, top-only DX10 DDS for one exact payload format. */
test_bytes fixture_dds_dx10_top(
    uint32_t       dxgi_format,
    const uint8_t *payload,
    size_t         payload_size,
    uint32_t       bytes_per_pixel);
test_bytes fixture_integer_overflow(void);
test_bytes fixture_png_rgba(void);
test_bytes fixture_png_flat(uint32_t side);
test_bytes fixture_png_rgba_gamma(void);
/* RGB with a suggested palette, a tRNS colour key and its zlib stream split over many IDATs. */
test_bytes fixture_png_rgb_keyed(void);
test_bytes fixture_tga_bgrx(void);
test_bytes fixture_jpeg_ycbcr(void);
test_bytes fixture_tiff_rgb(void);

/**
 * The controlled source the GPU conversions are measured against: nine by five, so a block row
 * hangs off both edges and the mip chain runs down to one pixel, with a colour ramp across it and
 * an alpha ramp through it. The independent reader rebuilds the same expectation from this file.
 */
test_bytes fixture_tga_gpu_gradient(void);

/**
 * Four flat quadrants, so every block of every runtime format repeats and the container has
 * something to compress. It is how the black-box test gets one GPU result stored both ways.
 */
test_bytes fixture_tga_gpu_flat(void);
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
    uint8_t                *output,
    size_t                  capacity,
    int                     big_endian,
    const fixture_tiff_tag *tags,
    size_t                  tag_count,
    const uint8_t          *trailing,
    size_t                  trailing_size);

/**
 * A JPEG around fixed all-ones quantisation and a three-symbol Huffman pair, so a test writes only
 * the frame shape it is about and the entropy bits it wants decoded.
 */
typedef struct fixture_jpeg_spec {
    uint8_t        frame_marker;
    uint8_t        precision;
    uint16_t       width;
    uint16_t       height;
    uint8_t        component_count;
    uint8_t        luma_sampling;
    uint16_t       restart_interval;
    const uint8_t *exif;
    size_t         exif_size;
    const uint8_t *entropy;
    size_t         entropy_size;
    int            omit_end_of_image;
} fixture_jpeg_spec;

size_t fixture_jpeg_build(uint8_t *output, size_t capacity, const fixture_jpeg_spec *spec);

/**
 * An uncompressed true-colour TGA of whatever size and samples a test wants. The GPU formats need
 * a controlled source of their own — a gradient wide enough for several blocks, a size that is not
 * a multiple of four, an alpha channel that is or is not used — and this is the cheapest source
 * format to build exactly: a header and the BGRA samples, with no compression in between.
 */
size_t fixture_tga_bytes(uint32_t width, uint32_t height, int with_alpha);
size_t fixture_tga_build(
    uint8_t       *output,
    size_t         capacity,
    uint32_t       width,
    uint32_t       height,
    int            with_alpha,
    const uint8_t *bgra);

#endif
