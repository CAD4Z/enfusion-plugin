/*
 * What every source-image codec in this converter shares: the decoded value it produces, the way
 * it refuses, and the two readers more than one format needs. A new format adds a decoder here,
 * not a second copy of the inflate, the byte order or the refusal shape.
 */
#ifndef EDDS_IMAGE_H
#define EDDS_IMAGE_H

#include <edds/edds.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct edds_decoded_mip {
    uint32_t width;
    uint32_t height;
    uint8_t *rgba;
} edds_decoded_mip;

/** One decoded source image, always straight RGBA8 with alpha declared by the source itself. */
typedef struct edds_decoded_source {
    uint32_t         width;
    uint32_t         height;
    int              has_alpha;
    uint8_t         *rgba;
    /** Zero for ordinary images; DDS supplies a complete chain including level zero. */
    uint32_t         supplied_mip_count;
    edds_decoded_mip supplied_mips[EDDS_MAX_MIPS];
} edds_decoded_source;

void edds_fail(edds_error *error, const char *code, const char *format, ...);

uint16_t edds_u16le(const uint8_t *at);
uint16_t edds_u16be(const uint8_t *at);
uint32_t edds_u32le(const uint8_t *at);
uint32_t edds_u32be(const uint8_t *at);
void edds_put_u32le(uint8_t *at, uint32_t value);

int edds_read_all(FILE *input, uint8_t **bytes, size_t *size, edds_error *error);

/** Exactly one zlib stream of a known decoded size, checksum included. PNG IDAT and TIFF Deflate. */
int edds_inflate_zlib(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size);

/**
 * Refuses a decoded size no mip pipeline would survive, before any of it is allocated. PNG and TGA
 * predate this and keep their own checks, whose refusals name the header field that overran.
 */
int edds_decoded_size_allowed(uint32_t width, uint32_t height);

edds_status edds_decode_png(FILE *input, edds_decoded_source *image, edds_error *error);
edds_status edds_decode_tga(FILE *input, edds_decoded_source *image, edds_error *error);
edds_status edds_decode_jpeg(FILE *input, edds_decoded_source *image, edds_error *error);
edds_status edds_decode_tiff(FILE *input, edds_decoded_source *image, edds_error *error);
edds_status edds_decode_dds(FILE *input, edds_decoded_source *image, edds_error *error);

#endif
