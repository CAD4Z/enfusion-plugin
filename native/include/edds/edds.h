#ifndef EDDS_EDDS_H
#define EDDS_EDDS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDDS_MAX_FILE_BYTES ((uint64_t)1024 * 1024 * 1024)
#define EDDS_MAX_DIMENSION 32768u
#define EDDS_MAX_MIPS 32u
#define EDDS_MAX_PREVIEW_BYTES ((uint32_t)64 * 1024 * 1024)
#define EDDS_MAX_LZ4_BLOCKS 1024u
#define EDDS_MAX_LZ4_STORED_BLOCK ((uint32_t)1024 * 1024)

typedef enum edds_status {
    EDDS_OK = 0,
    EDDS_INVALID_INVOCATION = 2,
    EDDS_INVALID_INPUT = 3,
    EDDS_UNSUPPORTED_FORMAT = 4,
    EDDS_CANCELLED = 5,
    EDDS_INTERNAL_FAILURE = 6
} edds_status;

typedef enum edds_container {
    EDDS_CONTAINER_COPY,
    EDDS_CONTAINER_LZ4
} edds_container;

typedef enum edds_pixel_format {
    EDDS_PIXEL_BGRA8,
    EDDS_PIXEL_BGRX8,
    EDDS_PIXEL_DXT1,
    EDDS_PIXEL_DXT5,
    EDDS_PIXEL_DXGI,
    EDDS_PIXEL_UNKNOWN
} edds_pixel_format;

typedef enum edds_source_format {
    EDDS_SOURCE_PNG,
    EDDS_SOURCE_TGA
} edds_source_format;

typedef enum edds_format_compress {
    EDDS_COMPRESS_COPY,
    EDDS_COMPRESS_FASTEST,
    EDDS_COMPRESS_MEDIUM,
    EDDS_COMPRESS_BEST
} edds_format_compress;

/** The exact supported Workbench slice. Values outside it are refused, never substituted. */
typedef struct edds_profile {
    edds_format_compress format_compress;
    uint32_t compress_threshold;
    int generate_mips;
} edds_profile;

#define EDDS_METADATA_GUID_BYTES 17u
#define EDDS_METADATA_PATH_BYTES 1024u

typedef struct edds_metadata {
    char guid[EDDS_METADATA_GUID_BYTES];
    char name[EDDS_METADATA_PATH_BYTES];
    char source_file[EDDS_METADATA_PATH_BYTES];
    edds_source_format source_format;
    edds_profile profile;
} edds_metadata;

typedef struct edds_error {
    char code[64];
    char message[256];
} edds_error;

typedef struct edds_mip {
    uint32_t level;
    uint32_t width;
    uint32_t height;
    edds_container container;
    uint32_t stored_bytes;
    uint32_t decoded_bytes;
    uint32_t block_count;
    uint64_t data_offset;
} edds_mip;

typedef struct edds_info {
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    uint32_t header_bytes;
    uint32_t flags;
    uint32_t pitch_or_linear_size;
    uint32_t depth;
    uint32_t pixel_format_flags;
    char four_cc[5];
    uint32_t rgb_bit_count;
    uint32_t r_mask;
    uint32_t g_mask;
    uint32_t b_mask;
    uint32_t a_mask;
    uint32_t caps;
    uint32_t caps2;
    uint32_t dxgi_format;
    uint32_t resource_dimension;
    uint32_t array_size;
    uint32_t misc_flag;
    int preview_supported;
    edds_pixel_format pixel_format;
    edds_mip mips[EDDS_MAX_MIPS];
} edds_info;

typedef int (*edds_cancelled_fn)(void *context);

/**
 * How far one conversion has got, from 0 to 1. A batch reports a file this way, so a long image
 * moves its own row rather than only appearing when it is finished.
 */
typedef void (*edds_progress_fn)(void *context, double progress);

edds_status edds_inspect(
    FILE *input,
    edds_info *info,
    edds_cancelled_fn cancelled,
    void *cancel_context,
    edds_error *error
);

edds_status edds_preview(
    FILE *input,
    const edds_info *info,
    uint32_t level,
    edds_cancelled_fn cancelled,
    void *cancel_context,
    uint8_t **rgba,
    size_t *rgba_size,
    edds_error *error
);

void edds_default_profile(edds_profile *profile);

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
);

edds_status edds_metadata_parse(FILE *input, edds_metadata *metadata, edds_error *error);
edds_status edds_metadata_write(FILE *output, const edds_metadata *metadata, edds_error *error);

void edds_free(void *allocation);
const char *edds_container_name(edds_container container);
const char *edds_status_category(edds_status status);

#ifdef __cplusplus
}
#endif

#endif
