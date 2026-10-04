/*
 * Synthetic TrueType fonts for the font tests, and the truth they were built from. Nothing here
 * reads a font: the fixture writes one from its own glyph table, and hands the same table — with
 * composite glyphs already composed — to the checks that rasterize it independently.
 */
#ifndef FONT_TEST_FIXTURE_H
#define FONT_TEST_FIXTURE_H

#include "fixture.h"

#include <stddef.h>
#include <stdint.h>

typedef enum font_fixture_variant {
    /** Every glyph kind; kerning in GPOS PairPos 1 and 2, one of them behind an Extension lookup. */
    FONT_FIXTURE_GPOS,
    /** The same glyphs; kerning only in a format 0 `kern` table, no cap height, short `loca`. */
    FONT_FIXTURE_KERN,
    /** An OpenType font with CFF outlines. */
    FONT_FIXTURE_CFF,
    /** The GPOS font with an `fvar` table: a variable font. */
    FONT_FIXTURE_VARIABLE,
    /** 7000 em-sized glyphs: more cells than one 4096 atlas holds at size 40. */
    FONT_FIXTURE_CROWDED
} font_fixture_variant;

#define FONT_FIXTURE_UNITS_PER_EM  1000u
#define FONT_FIXTURE_CROWDED_FIRST 0x4E00u
#define FONT_FIXTURE_CROWDED_COUNT 7000u

test_bytes font_fixture(font_fixture_variant variant);

typedef struct font_fixture_point {
    double x;
    double y;
    int    on_curve;
} font_fixture_point;

/** One character's outline in font units, composites composed, in TrueType point form. */
typedef struct font_fixture_outline {
    font_fixture_point *points;
    size_t              point_count;
    /** Index one past the last point of each contour. */
    size_t             *contour_ends;
    size_t              contour_count;
    /** The advance the font gives the character, after any USE_MY_METRICS. */
    int                 advance;
} font_fixture_outline;

/** Characters the font maps, ascending. */
size_t font_fixture_codes(font_fixture_variant variant, const uint32_t **codes);

int font_fixture_outline_of(font_fixture_variant variant, uint32_t code, font_fixture_outline *outline);
void font_fixture_outline_free(font_fixture_outline *outline);

/** `OS/2.sCapHeight`, or -1 when the font has none and the height of H has to stand in. */
int font_fixture_cap_height(font_fixture_variant variant);

typedef struct font_fixture_pair {
    uint32_t left;
    uint32_t right;
    /** Font units, as planted: the XAdvance of the first glyph. */
    int      value;
} font_fixture_pair;

/**
 * The pairs a correct reader takes from the font, before rounding: only the `kern` feature, only
 * the default language systems of DFLT, latn and cyrl, the first matching subtable of each lookup.
 * Pairs the font also holds and a correct reader leaves out — another script, a language's own
 * system, another feature, a later subtable — are not listed.
 */
size_t font_fixture_pairs(font_fixture_variant variant, const font_fixture_pair **pairs);

/**
 * The frame a generator draws for a missing U+25A1, in em: what the spec of the generator says,
 * written down here independently of it.
 */
typedef struct font_fixture_frame {
    double left;
    double bottom;
    double side;
    double stroke;
    double advance;
} font_fixture_frame;

font_fixture_frame font_fixture_missing_box(void);

/** An em-relative advance the generator gives a space it had to make up. */
#define FONT_FIXTURE_DRAWN_SPACE_ADVANCE 0.25

#endif
