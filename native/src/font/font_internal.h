/*
 * The parts of the font generator, each behind the narrowest interface the next one needs: the
 * TrueType reader hands out contours and kerning, the shape turns contours into the boundary of
 * what they fill, the field renders that boundary into one atlas cell, and the FNT writer records
 * where every cell went.
 */
#ifndef FONT_INTERNAL_H
#define FONT_INTERNAL_H

#include <font/font.h>

#include <stddef.h>
#include <stdint.h>

/** Hard limits on what one glyph may cost, checked while it is read. */
#define FONT_MAX_GLYPH_POINTS    16384u
#define FONT_MAX_GLYPH_CONTOURS  4096u
#define FONT_MAX_COMPONENT_DEPTH 16u
#define FONT_MAX_COMPONENTS      4096u

/** Hard limits on kerning: the pairs among the characters of one set, and PairPos subtables. */
#define FONT_MAX_PAIRS          ((size_t)1 << 20)
#define FONT_MAX_PAIR_SUBTABLES 4096u

/** Fills `error` with a code and a printf-style message; nothing happens when `error` is NULL. */
void font_fail(edds_error *error, const char *code, const char *format, ...);

/* --- The TrueType file ------------------------------------------------------------------------ */

/** Where one table lies in the file, and whether the file has it at all. */
typedef struct font_table {
    uint32_t offset;
    uint32_t length;
    int      present;
} font_table;

/** A TrueType file opened by font_face_open: where its tables lie, and what they say. */
typedef struct font_face {
    /** The file, as the caller passed it. */
    const uint8_t *data;
    size_t         size;

    /** The tables the reader uses, as the table directory places them. */
    font_table head, hhea, hmtx, maxp, cmap, loca, glyf, os2, name, gpos, kern;

    /** Font units per em, and whether loca holds 32-bit offsets or 16-bit ones halved. */
    uint32_t units_per_em;
    int      long_offsets;

    /** The number of glyphs, and how many of them have their own advance in hmtx. */
    uint32_t glyph_count;
    uint32_t metric_count;

    /** The chosen character map subtable, absolute in the file, and the bytes it may span. */
    uint32_t cmap_offset;
    uint32_t cmap_limit;
    unsigned cmap_format;

    /** OS/2.sCapHeight in font units, or -1 when the font does not carry one. */
    int cap_height;
} font_face;

/**
 * Opens a TrueType file held in memory: finds its tables, checks the ones every glyph depends on
 * and chooses a character map. CFF outlines, font collections and variable fonts are refused.
 */
edds_status font_face_open(font_face *face, const uint8_t *data, size_t size, edds_error *error);

/** The glyph a code point maps to, or 0 when the font has none. */
uint32_t font_face_glyph(const font_face *face, uint32_t code);

/** The advance width of a glyph, in font units. */
uint32_t font_face_advance(const font_face *face, uint32_t glyph);

/** UTF-8 family and style, typographic names first. */
void font_face_names(const font_face *face, font_source_info *info);

/** One point of a contour, in font units, and whether it lies on the curve. */
typedef struct font_point {
    double x;
    double y;
    int    on_curve;
} font_point;

/** TrueType contours in font units, composite glyphs composed. */
typedef struct font_contours {
    /** The points of every contour, one contour after another. */
    font_point *points;
    size_t      count;
    size_t      capacity;

    /** Index one past the last point of each contour. */
    size_t *ends;
    size_t  contour_count;
    size_t  contour_capacity;
} font_contours;

/**
 * Reads the outline of one glyph into `contours` and its advance into `advance`. On success the
 * caller releases the contours with font_contours_free; on failure nothing is left to release.
 */
edds_status font_face_contours(
    const font_face *face,
    uint32_t         glyph,
    font_contours   *contours,
    uint32_t        *advance,
    edds_error      *error);

/** Releases the points and ends of contours and leaves them empty; NULL is ignored. */
void font_contours_free(font_contours *contours);

/** One kerning pair: two BMP characters, and the shift of the first one's advance. */
typedef struct font_pair {
    uint16_t left;
    uint16_t right;
    int32_t  value;
} font_pair;

/**
 * Kerning between the given characters, in whole atlas pixels: GPOS PairPos from the `kern`
 * feature of the default language systems of DFLT, latn and cyrl, or a format 0 `kern` table when
 * there is no GPOS. Only BMP characters, no zero values, ordered by `(left << 16) | right`.
 * On success the caller releases `*pairs` with free.
 */
edds_status font_face_kerning(
    const font_face *face,
    const uint32_t  *codes,
    const uint32_t  *glyphs,
    size_t           count,
    double           scale,
    font_pair      **pairs,
    size_t          *pair_count,
    edds_error      *error);

/* --- Shapes ----------------------------------------------------------------------------------- */

/** A point or a direction in the plane. */
typedef struct font_vec {
    double x;
    double y;
} font_vec;

/** A line from p[0] to p[2], or a quadratic Bezier with control point p[1]. */
typedef struct font_edge {
    font_vec p[3];
    int      quad;
    /** Channels of the multi-channel field this edge feeds: 1 red, 2 green, 4 blue. */
    unsigned color;
} font_edge;

/** Edges in closed loops: loop `n` runs from `loop_ends[n - 1]` (or 0) to `loop_ends[n]`. */
typedef struct font_shape {
    /** The edges of every loop, one loop after another. */
    font_edge *edges;
    size_t     count;
    size_t     capacity;

    /** Index one past the last edge of each loop. */
    size_t *loop_ends;
    size_t  loop_count;
    size_t  loop_capacity;
} font_shape;

/**
 * TrueType contours as lines and quadratic segments, scaled; degenerate segments are dropped.
 * The caller releases `shape` with font_shape_free, even when this fails.
 */
edds_status font_shape_of_contours(
    const font_contours *contours,
    double               scale,
    font_shape          *shape,
    edds_error          *error);

/**
 * The boundary of the area the shape fills under the nonzero rule, as closed loops with the inside
 * on the right of every edge. Parts of contours inside other contours are no longer edges.
 */
edds_status font_shape_union(const font_shape *shape, font_shape *boundary, edds_error *error);

/**
 * Exact extent of the curves; 0 for an empty shape. `box` gets the least x and y, then the
 * greatest x and y.
 */
int font_shape_bounds(const font_shape *shape, double box[4]);

/** Nonzero-rule winding number of a point against every edge of the shape. */
int font_shape_winding(const font_shape *shape, font_vec point);

/** Appends an edge to the open loop; 0 when memory runs out. */
int font_shape_push(font_shape *shape, const font_edge *edge);

/** Releases the edges and loop ends of a shape and leaves it empty; NULL is ignored. */
void font_shape_free(font_shape *shape);

/* --- The field -------------------------------------------------------------------------------- */

/** Where one glyph lands: its cell in the atlas and its box, in whole atlas pixels. */
typedef struct font_placement {
    /** The cell: its left and top edges in the atlas, and its side. */
    uint32_t cell_x;
    uint32_t cell_y;
    uint32_t cell;

    /** The box: its left and top edges, its width and its height. */
    int32_t  box_x;
    int32_t  box_y;
    uint32_t width;
    uint32_t height;
} font_placement;

/**
 * Colours the boundary and renders its multi-channel field into the glyph's cell of an RGBA atlas,
 * placed so that the box the engine centres in the cell holds the outline exactly where it is.
 * `shape` is the original outline: the inside is decided by its winding, not by the boundary.
 * `lost` gets the area in square atlas pixels that the outline fills and the shader leaves out:
 * strokes thinner than a texel, where they fall between texel centres.
 */
edds_status font_field_render(
    font_shape           *boundary,
    const font_shape     *shape,
    const font_placement *placement,
    uint8_t              *atlas,
    uint32_t              atlas_width,
    double               *lost,
    edds_error           *error);

/* --- FNT5 ------------------------------------------------------------------------------------- */

/** One character as the FNT file records it. */
typedef struct font_entry {
    /** The character. */
    uint32_t code;

    /** The left and top edges of its cell in the atlas, and the width and height of its box. */
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;

    /** The left and top edges of its box, and its advance, in whole atlas pixels. */
    int16_t box_x;
    int16_t box_y;
    int16_t advance;
} font_entry;

/** The values the HEAD chunk is written from. */
typedef struct font_header {
    const char *name;
    uint32_t    size;
    uint32_t    cell;
    float       cap_height;
} font_header;

/**
 * Writes a whole FNT5 file: HEAD from `header`, GLPS and TCRD from the entries, KERN from the
 * pairs. On success the caller releases `*bytes` with free, and `*range_count` holds the number
 * of runs GLPS lists.
 */
edds_status font_fnt_write(
    const font_header *header,
    const font_entry  *entries,
    size_t             entry_count,
    const font_pair   *pairs,
    size_t             pair_count,
    uint8_t          **bytes,
    size_t            *size,
    uint32_t          *range_count,
    edds_error        *error);

#endif
