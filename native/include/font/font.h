#ifndef FONT_FONT_H
#define FONT_FONT_H

/*
 * SDF fonts for the Enfusion engine, generated from a static TrueType font: the FNT5 file the
 * engine reads glyph boxes, ranges and kerning from, and the RGBA atlas holding one multi-channel
 * signed distance field per glyph. Statuses and errors are those of the EDDS core, so both areas
 * of the executable report through one envelope with one set of exit categories.
 */

#include <edds/edds.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Atlas pixels per em the engine accepts with one edge formula. Above 40 it derives the edge width
 * from the size itself, which a field sized for `R` would no longer match.
 */
#define FONT_MIN_SIZE     8u
#define FONT_MAX_SIZE     40u
#define FONT_DEFAULT_SIZE 32u

/** The `R` every generated font declares in its header. */
#define FONT_FIELD_R 8

/**
 * Atlas pixels over which the field runs from 0 to 1: 1.5 × R / √2. The engine's shader turns that
 * into an edge exactly one screen pixel wide at the default text sharpness of 1.
 */
#define FONT_FIELD_RANGE 8.4852813742385702

#define FONT_MAX_ATLAS                4096u
#define FONT_MAX_FILE_BYTES           ((uint32_t)64 * 1024 * 1024)
#define FONT_MAX_CHARACTERS           8192u
#define FONT_MAX_CHARACTER_FILE_BYTES ((uint32_t)1024 * 1024)

/** The two characters every font carries: the space and the box the engine draws for a miss. */
#define FONT_SPACE       0x20u
#define FONT_MISSING_BOX 0x25A1u

/**
 * The recipe a `.fnt.meta` file holds next to its font, the way `.edds.meta` holds a texture's.
 * `source_file` and `characters` are relative to the folder of the recipe; an empty `characters`
 * means the built-in set.
 */
typedef struct font_recipe {
    char     guid[EDDS_METADATA_GUID_BYTES];
    char     name[EDDS_METADATA_PATH_BYTES];
    char     source_file[EDDS_METADATA_PATH_BYTES];
    char     characters[EDDS_METADATA_PATH_BYTES];
    uint32_t font_size;
} font_recipe;

/**
 * Reads a whole recipe. A missing `FontSize` is the default; unknown fields and other platforms
 * are left behind, since the recipe is always written back in canonical form.
 */
edds_status font_recipe_parse(FILE *input, font_recipe *recipe, edds_error *error);

/** Only the GUID, for a recipe about to be replaced by a new one. */
edds_status font_recipe_guid(FILE *input, char guid[EDDS_METADATA_GUID_BYTES], edds_error *error);

edds_status font_recipe_write(FILE *output, const font_recipe *recipe, edds_error *error);

/** Distinct code points in ascending order. */
typedef struct font_characters {
    uint32_t *codes;
    size_t    count;
} font_characters;

/**
 * A character-set file: UTF-8 text, every character of which belongs to the set. Line breaks and
 * other control characters separate, they are not members; the space is always in a font anyway.
 */
edds_status font_characters_parse(
    const uint8_t   *text,
    size_t           size,
    font_characters *characters,
    edds_error      *error);

/** Basic Latin, Latin-1 and Cyrillic U+0400–U+045F. */
edds_status font_characters_builtin(font_characters *characters, edds_error *error);

void font_characters_free(font_characters *characters);

/** What a TrueType file says about itself, for naming the font made from it. */
typedef struct font_source_info {
    char     family[128];
    char     style[128];
    uint32_t units_per_em;
    uint32_t glyph_count;
} font_source_info;

edds_status font_source_describe(
    const uint8_t    *data,
    size_t            size,
    font_source_info *info,
    edds_error       *error);

typedef struct font_request {
    const uint8_t         *data;
    size_t                 size;
    /** NULL for the built-in set. */
    const font_characters *characters;
    uint32_t               font_size;
    /** The name the FNT header carries: the file name of the font without its extension. */
    const char            *name;
} font_request;

typedef struct font_output {
    uint8_t         *fnt;
    size_t           fnt_size;
    /** RGBA8, top to bottom, alpha 255 everywhere. */
    uint8_t         *atlas;
    uint32_t         atlas_width;
    uint32_t         atlas_height;
    uint32_t         cell;
    uint32_t         glyph_count;
    uint32_t         range_count;
    uint32_t         pair_count;
    /** Requested characters the font has no glyph for; they are left out of the result. */
    uint32_t        *missing;
    size_t           missing_count;
    /** Mandatory characters the font lacks, which the generator drew itself. */
    uint32_t        *drawn;
    size_t           drawn_count;
    font_source_info source;
} font_output;

edds_status font_generate(
    const font_request *request,
    font_output        *output,
    edds_cancelled_fn   cancelled,
    void               *cancel_context,
    edds_progress_fn    progress,
    void               *progress_context,
    edds_error         *error);

void font_output_free(font_output *output);

typedef struct font_range {
    uint32_t first;
    uint32_t count;
} font_range;

/**
 * The header and tables of an FNT5 file, as the engine would read them. The header's three floats
 * are `cap_height` (A), `line_height` (B, the step from one line to the next) and `c` (C, which a
 * generated font sets to its size like B); `r` is the field range R the shader scales edges by.
 */
typedef struct font_info {
    char        name[256];
    int32_t     size;
    uint8_t     type;
    int32_t     cell;
    float       cap_height;
    float       line_height;
    float       c;
    int16_t     r;
    uint8_t     bold;
    uint8_t     italic;
    uint32_t    glyph_count;
    uint32_t    range_count;
    uint32_t    pair_count;
    font_range *ranges;
} font_info;

edds_status font_inspect(FILE *input, font_info *info, edds_error *error);
void font_info_free(font_info *info);

#ifdef __cplusplus
}
#endif

#endif
