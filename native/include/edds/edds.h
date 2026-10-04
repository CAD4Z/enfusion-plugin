#ifndef EDDS_EDDS_H
#define EDDS_EDDS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDDS_MAX_FILE_BYTES       ((uint64_t)1024 * 1024 * 1024)
#define EDDS_MAX_DIMENSION        32768u
#define EDDS_MAX_MIPS             32u
#define EDDS_MAX_PREVIEW_BYTES    ((uint32_t)64 * 1024 * 1024)
#define EDDS_MAX_LZ4_BLOCKS       1024u
#define EDDS_MAX_LZ4_STORED_BLOCK ((uint32_t)1024 * 1024)

typedef enum edds_status {
    EDDS_OK                 = 0,
    EDDS_INVALID_INVOCATION = 2,
    EDDS_INVALID_INPUT      = 3,
    EDDS_UNSUPPORTED_FORMAT = 4,
    EDDS_CANCELLED          = 5,
    EDDS_INTERNAL_FAILURE   = 6
} edds_status;

typedef enum edds_container {
    EDDS_CONTAINER_COPY,
    EDDS_CONTAINER_LZ4
} edds_container;

typedef enum edds_pixel_format {
    EDDS_PIXEL_BGRA8,
    EDDS_PIXEL_BGRX8,
    EDDS_PIXEL_R8,
    EDDS_PIXEL_RG8,
    EDDS_PIXEL_DXT1,
    EDDS_PIXEL_DXT5,
    EDDS_PIXEL_BC4,
    EDDS_PIXEL_BC5,
    EDDS_PIXEL_BC7,
    EDDS_PIXEL_DXGI,
    EDDS_PIXEL_UNKNOWN
} edds_pixel_format;

/** The wire name of a runtime format, and the channels a decode of it actually carries. */
const char *edds_pixel_format_name(edds_pixel_format format);
const char *edds_pixel_format_channels(edds_pixel_format format);

typedef enum edds_source_format {
    EDDS_SOURCE_PNG,
    EDDS_SOURCE_TGA,
    EDDS_SOURCE_JPG,
    EDDS_SOURCE_TIFF,
    EDDS_SOURCE_DDS
} edds_source_format;

/**
 * One row of the source contract: a DayZ Workbench texture resource class, the single extension
 * that names it, and the wire name the machine protocol reports for it. Extensions that only alias
 * a format are absent on purpose — Workbench registers `.jpg` and `.tiff`, not `.jpeg` and `.tif`,
 * so a file under an alias would convert into an EDDS the editor calls registered and Workbench
 * does not see a source for. Everything in the converter that has to know which inputs exist —
 * the CLI extension check, the metadata resource class, the decoder dispatch — reads this table.
 */
typedef struct edds_source_capability {
    edds_source_format format;
    const char        *extension;
    const char        *wire_name;
    const char        *resource_class;
} edds_source_capability;

const edds_source_capability *edds_source_capabilities(size_t *count);
const edds_source_capability *edds_source_capability_of_format(edds_source_format format);
const edds_source_capability *edds_source_capability_of_resource_class(const char *resource_class);

typedef enum edds_format_compress {
    EDDS_COMPRESS_COPY,
    EDDS_COMPRESS_FASTEST,
    EDDS_COMPRESS_MEDIUM,
    EDDS_COMPRESS_BEST
} edds_format_compress;

/** Workbench `Conversion`: which GPU format the RGBA samples are turned into. */
typedef enum edds_conversion {
    EDDS_CONVERSION_NONE,
    EDDS_CONVERSION_DXT,
    EDDS_CONVERSION_RED,
    EDDS_CONVERSION_RED_HQ,
    EDDS_CONVERSION_RED_GREEN,
    EDDS_CONVERSION_RED_GREEN_HQ,
    EDDS_CONVERSION_COLOR_HQ,
    EDDS_CONVERSION_HDR
} edds_conversion;

/**
 * `ConversionQuality` is a fraction of one, and DayZ's own metadata writes it to three decimals
 * (`0.026`, `0.403`, `0.497`, `1`). Carrying it as thousandths keeps every value the corpus uses
 * exact through the CLI, the batch protocol and the metadata text, where a binary fraction would
 * have to be rounded on the way out and would no longer be the value that came in.
 */
#define EDDS_QUALITY_SCALE 1000u

/**
 * One row of the conversion contract: the Workbench enum, the wire name the CLI and the machine
 * protocol use for it, whether this converter implements it, and whether `ConversionQuality`
 * reaches its encoder at all — Workbench itself describes the field as "Conversion quality for
 * compressed formats", so an uncompressed conversion refuses a quality other than the default
 * rather than accepting a number that would change nothing.
 */
typedef struct edds_conversion_capability {
    edds_conversion conversion;
    const char     *workbench_name;
    const char     *wire_name;
    int             supported;
    int             uses_quality;
} edds_conversion_capability;

const edds_conversion_capability *edds_conversions(size_t *count);
const edds_conversion_capability *edds_conversion_capability_of(edds_conversion conversion);
const edds_conversion_capability *edds_conversion_of_workbench_name(const char *name);
const edds_conversion_capability *edds_conversion_of_wire_name(const char *name);

typedef enum edds_swizzling {
    EDDS_SWIZZLE_NONE,
    EDDS_SWIZZLE_TERRAIN_LAYER,
    EDDS_SWIZZLE_TERRAIN_SUPER,
    EDDS_SWIZZLE_ALPHA_TO_RGB,
    EDDS_SWIZZLE_SMDI_TO_GS,
    EDDS_SWIZZLE_NORMAL_NOHQ,
    EDDS_SWIZZLE_NORMAL_GA,
    EDDS_SWIZZLE_TERRAIN_NORMAL,
    EDDS_SWIZZLE_NORMAL_SPECULAR,
    EDDS_SWIZZLE_AMBIENT_SPECULAR,
    EDDS_SWIZZLE_UNKNOWN
} edds_swizzling;

/** Exact Workbench names and stable CLI spellings; no filename inference. */
typedef struct edds_swizzle_capability {
    edds_swizzling swizzling;
    const char    *workbench_name;
    const char    *wire_name;
    int            writes_alpha;
} edds_swizzle_capability;

const edds_swizzle_capability *edds_swizzles(size_t *count);
const edds_swizzle_capability *edds_swizzle_capability_of(edds_swizzling swizzling);
const edds_swizzle_capability *edds_swizzle_of_workbench_name(const char *name);

typedef enum edds_mipmap_function {
    EDDS_MIPMAP_FILTER,
    EDDS_MIPMAP_NORMALIZE,
    EDDS_MIPMAP_COLOR_NOISE
} edds_mipmap_function;

typedef enum edds_mipmap_filter {
    EDDS_FILTER_BOX,
    EDDS_FILTER_KAISER,
    EDDS_FILTER_TRIANGLE
} edds_mipmap_filter;

/** The exact supported Workbench slice. Values outside it are refused, never substituted. */
typedef struct edds_profile {
    edds_format_compress format_compress;
    uint32_t             compress_threshold;
    edds_conversion      conversion;
    edds_swizzling       swizzling;
    /** Thousandths of one, so 1000 is Workbench's default `ConversionQuality 1`. */
    uint32_t             conversion_quality;
    /** Number of the largest completed levels removed after generation or supplied-mip decode. */
    uint32_t             remove_mips;
    int                  contains_mips;
    int                  generate_mips;
    int                  normalize;
    edds_mipmap_function mipmap_function;
    edds_mipmap_filter   mipmap_filter;
    int                  tiled_texture;
} edds_profile;

#define EDDS_METADATA_GUID_BYTES 17u
#define EDDS_METADATA_PATH_BYTES 1024u

typedef struct edds_metadata {
    char               guid[EDDS_METADATA_GUID_BYTES];
    char               name[EDDS_METADATA_PATH_BYTES];
    char               source_file[EDDS_METADATA_PATH_BYTES];
    edds_source_format source_format;
    edds_profile       profile;
} edds_metadata;

typedef struct edds_error {
    char code[64];
    char message[256];
} edds_error;

typedef struct edds_mip {
    uint32_t       level;
    uint32_t       width;
    uint32_t       height;
    edds_container container;
    uint32_t       stored_bytes;
    uint32_t       decoded_bytes;
    uint32_t       block_count;
    uint64_t       data_offset;
} edds_mip;

typedef struct edds_info {
    uint32_t          width;
    uint32_t          height;
    uint32_t          mip_count;
    uint32_t          header_bytes;
    uint32_t          flags;
    uint32_t          pitch_or_linear_size;
    uint32_t          depth;
    uint32_t          pixel_format_flags;
    char              four_cc[5];
    uint32_t          rgb_bit_count;
    uint32_t          r_mask;
    uint32_t          g_mask;
    uint32_t          b_mask;
    uint32_t          a_mask;
    uint32_t          caps;
    uint32_t          caps2;
    uint32_t          dxgi_format;
    uint32_t          resource_dimension;
    uint32_t          array_size;
    uint32_t          misc_flag;
    int               preview_supported;
    edds_pixel_format pixel_format;
    edds_mip          mips[EDDS_MAX_MIPS];
} edds_info;

typedef int (*edds_cancelled_fn)(void *context);

/**
 * How far one conversion has got, from 0 to 1. A batch reports a file this way, so a long image
 * moves its own row rather than only appearing when it is finished.
 */
typedef void (*edds_progress_fn)(void *context, double progress);

edds_status edds_inspect(
    FILE             *input,
    edds_info        *info,
    edds_cancelled_fn cancelled,
    void             *cancel_context,
    edds_error       *error);

edds_status edds_preview(
    FILE             *input,
    const edds_info  *info,
    uint32_t          level,
    edds_cancelled_fn cancelled,
    void             *cancel_context,
    uint8_t         **rgba,
    size_t           *rgba_size,
    edds_error       *error);

void edds_default_profile(edds_profile *profile);

/** Whether a profile is inside this converter's slice, with the refusal when it is not. */
edds_status edds_profile_check(const edds_profile *profile, edds_error *error);

/** How much alpha one source image actually carries, which is not the same as declaring one. */
typedef enum edds_source_alpha {
    EDDS_ALPHA_ABSENT,
    EDDS_ALPHA_OPAQUE,
    EDDS_ALPHA_USED
} edds_source_alpha;

/** The runtime format a profile produces for a source carrying that much alpha. */
edds_pixel_format edds_profile_pixel_format(const edds_profile *profile, edds_source_alpha alpha);

edds_status edds_convert(
    FILE               *source,
    edds_source_format  source_format,
    FILE               *output,
    const edds_profile *profile,
    edds_cancelled_fn   cancelled,
    void               *cancel_context,
    edds_progress_fn    progress,
    void               *progress_context,
    edds_error         *error);

/**
 * Encodes one top-to-bottom RGBA8 image that already exists in memory — a generated atlas — through
 * the same profile contract, runtime formats and container writer as a converted source file.
 * `has_alpha` is what the image declares, exactly as a decoder would report it for a file.
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
    edds_error         *error);

edds_status edds_metadata_parse(FILE *input, edds_metadata *metadata, edds_error *error);
edds_status edds_metadata_write(FILE *output, const edds_metadata *metadata, edds_error *error);

void edds_free(void *allocation);
const char *edds_container_name(edds_container container);
const char *edds_status_category(edds_status status);

#ifdef __cplusplus
}
#endif

#endif
