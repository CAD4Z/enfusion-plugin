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
 * Atlas pixels over which the field runs from 0 to 1: 1.5 * R / sqrt(2). The engine's shader
 * turns that into an edge exactly one screen pixel wide at the default text sharpness of 1.
 */
#define FONT_FIELD_RANGE 8.4852813742385702

/** The widest and the tallest an atlas may be, in pixels. */
#define FONT_MAX_ATLAS 4096u

/** The largest TrueType or FNT file that is read. */
#define FONT_MAX_FILE_BYTES ((uint32_t)64 * 1024 * 1024)

/** The most distinct characters a character set may hold. */
#define FONT_MAX_CHARACTERS 8192u

/** The largest character-set file that is read. */
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
    /** The two parts of `Name "{GUID}path"`: the GUID, and the resource path after it. */
    char guid[EDDS_METADATA_GUID_BYTES];
    char name[EDDS_METADATA_PATH_BYTES];

    /** `SourceFile`, `Characters` and `FontSize` of the PC configuration. */
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

/**
 * Writes a recipe in canonical form: its `Name`; the PC configuration with `SourceFile`,
 * `Characters` unless it is empty, and `FontSize`; and XBOX_ONE, PS4 and LINUX, each an empty
 * block inheriting PC.
 */
edds_status font_recipe_write(FILE *output, const font_recipe *recipe, edds_error *error);

/** Distinct code points in ascending order. */
typedef struct font_characters {
    uint32_t *codes;
    size_t    count;
} font_characters;

/**
 * A character-set file: UTF-8 text, every character of which belongs to the set. Line breaks and
 * other control characters separate, they are not members; the space is always in a font anyway.
 * The caller releases the set with font_characters_free.
 */
edds_status font_characters_parse(const uint8_t *text, size_t size, font_characters *characters, edds_error *error);

/**
 * Basic Latin, Latin-1 and Cyrillic U+0400-U+045F. The caller releases the set with
 * font_characters_free, as it does a parsed one.
 */
edds_status font_characters_builtin(font_characters *characters, edds_error *error);

/** Releases the codes of a set and leaves it empty; NULL is ignored. */
void font_characters_free(font_characters *characters);

/** What a TrueType file says about itself, for naming the font made from it. */
typedef struct font_source_info {
    char     family[128];
    char     style[128];
    uint32_t units_per_em;
    uint32_t glyph_count;
} font_source_info;

/** Opens a TrueType file only to fill `info` with its names, units per em and glyph count. */
edds_status font_source_describe(const uint8_t *data, size_t size, font_source_info *info, edds_error *error);

/** What font_generate makes a font from. */
typedef struct font_request {
    /** The bytes of the TrueType file. */
    const uint8_t *data;
    size_t         size;

    /** NULL for the built-in set. */
    const font_characters *characters;

    /** Atlas pixels per em, from FONT_MIN_SIZE to FONT_MAX_SIZE. */
    uint32_t font_size;

    /** The name the FNT header carries: the file name of the font without its extension. */
    const char *name;
} font_request;

/** What font_generate makes: the FNT file, the atlas, and what went into them. */
typedef struct font_output {
    /** The FNT file. */
    uint8_t *fnt;
    size_t   fnt_size;

    /** RGBA8, top to bottom, alpha 255 everywhere. */
    uint8_t *atlas;
    uint32_t atlas_width;
    uint32_t atlas_height;

    /** The side of the square cell each glyph has in the atlas, in pixels. */
    uint32_t cell;

    /** What the FNT file holds: glyphs, runs of consecutive codes, kerning pairs. */
    uint32_t glyph_count;
    uint32_t range_count;
    uint32_t pair_count;

    /** Requested characters the font has no glyph for; they are left out of the result. */
    uint32_t *missing;
    size_t    missing_count;

    /** Mandatory characters the font lacks, which the generator drew itself. */
    uint32_t *drawn;
    size_t    drawn_count;

    /** What the TrueType file says about itself. */
    font_source_info source;
} font_output;

/**
 * Generates one font from `request`: the FNT file and the atlas. `cancelled` is asked as the glyphs
 * are prepared and drawn, and `progress` hears how far the work has got; either may be NULL. On
 * success the caller owns the buffers of `output` and releases them with font_output_free; on
 * failure nothing is left to release.
 */
edds_status font_generate(
    const font_request *request,
    font_output        *output,
    edds_cancelled_fn   cancelled,
    void               *cancel_context,
    edds_progress_fn    progress,
    void               *progress_context,
    edds_error         *error);

/** Releases the buffers of an output and leaves it empty; NULL is ignored. */
void font_output_free(font_output *output);

/** One run of GLPS: `count` consecutive codes, starting at `first`. */
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
    /** What HEAD says. */
    char    name[256];
    int32_t size;
    uint8_t type;
    int32_t cell;
    float   cap_height;
    float   line_height;
    float   c;
    int16_t r;
    uint8_t bold;
    uint8_t italic;

    /** How many glyphs GLPS lists and in how many runs, and how many pairs KERN holds. */
    uint32_t glyph_count;
    uint32_t range_count;
    uint32_t pair_count;

    /** The runs GLPS lists, `range_count` of them. */
    font_range *ranges;
} font_info;

/**
 * Reads an FNT5 file into `info`, refusing one without exactly one HEAD, GLPS and TCRD or whose
 * chunks do not add up. On success the caller releases `info` with font_info_free; on failure
 * nothing is left to release.
 */
edds_status font_inspect(FILE *input, font_info *info, edds_error *error);

/** Releases the runs of an info; NULL is ignored. */
void font_info_free(font_info *info);

#ifdef __cplusplus
}
#endif

#endif
