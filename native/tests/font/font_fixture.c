#include "font_fixture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * The glyph table every variant is written from. Simple glyphs are TrueType contours: outer ones
 * clockwise, holes counter-clockwise, on- and off-curve points as a font holds them. Composite
 * glyphs name their components the way `glyf` does, transform included.
 */

#define P(x, y) { (x), (y), 1 }
#define Q(x, y) { (x), (y), 0 }
/* Clockwise, the way TrueType writes an outer contour. */
#define RECT(x0, y0, x1, y1) P(x0, y0), P(x0, y1), P(x1, y1), P(x1, y0)
/* Counter-clockwise: a hole. */
#define HOLE(x0, y0, x1, y1) P(x0, y0), P(x1, y0), P(x1, y1), P(x0, y1)

enum {
    GID_NOTDEF,
    GID_SPACE,
    GID_H,
    GID_TSE,
    GID_O,
    GID_A,
    GID_V,
    GID_T,
    GID_O_SMALL,
    GID_A_SMALL,
    GID_DIERESIS,
    GID_A_DIERESIS,
    GID_ORDINAL,
    GID_TURNED_A,
    GID_A_DIERESIS_MACRON,
    GID_MACRON,
    GID_EMOJI,
    GID_A_RING,
    GID_RING,
    GID_COUNT
};

enum {
    ARG_1_AND_2_ARE_WORDS = 0x0001,
    ARGS_ARE_XY_VALUES = 0x0002,
    WE_HAVE_A_SCALE = 0x0008,
    MORE_COMPONENTS = 0x0020,
    WE_HAVE_AN_X_AND_Y_SCALE = 0x0040,
    WE_HAVE_A_TWO_BY_TWO = 0x0080,
    USE_MY_METRICS = 0x0200
};

typedef struct simple_glyph {
    const font_fixture_point *points;
    size_t point_count;
    const size_t *ends;
    size_t contour_count;
} simple_glyph;

typedef struct component {
    uint16_t glyph;
    /* ARG_1_AND_2_ARE_WORDS, ARGS_ARE_XY_VALUES, USE_MY_METRICS and the transform flag. */
    uint16_t flags;
    int arg1;
    int arg2;
    /* x' = a·x + c·y + dx, y' = b·x + d·y + dy */
    double a, b, c, d;
} component;

typedef struct glyph_def {
    int advance;
    const simple_glyph *simple;
    const component *components;
    size_t component_count;
} glyph_def;

/* Taller than the other capitals, so a cap height taken from H differs from the 0.7 em fallback. */
static const font_fixture_point h_points[] = {
    RECT(80, 0, 180, 720), RECT(520, 0, 620, 720), RECT(100, 300, 600, 400)
};
static const size_t h_ends[] = { 4, 8, 12 };
static const simple_glyph h_glyph = { h_points, 12, h_ends, 3 };

/* Ц the way type designers build it: stems, foot and tail overlapping instead of joined. */
static const font_fixture_point tse_points[] = {
    RECT(80, 0, 180, 700), RECT(520, 0, 620, 700), RECT(80, 0, 680, 100), RECT(600, -180, 680, 100)
};
static const size_t tse_ends[] = { 4, 8, 12, 16 };
static const simple_glyph tse_glyph = { tse_points, 16, tse_ends, 4 };

/* An outer contour of off-curve points only, around a hole of alternating points. */
static const font_fixture_point o_points[] = {
    Q(350, 670), Q(576, 576), Q(670, 350), Q(576, 124), Q(350, 30), Q(124, 124), Q(30, 350), Q(124, 576),
    P(520, 350), Q(520, 520), P(350, 520), Q(180, 520), P(180, 350), Q(180, 180), P(350, 180), Q(520, 180)
};
static const size_t o_ends[] = { 8, 16 };
static const simple_glyph o_glyph = { o_points, 16, o_ends, 2 };

static const font_fixture_point a_points[] = {
    P(0, 0), P(250, 700), P(350, 700), P(600, 0), P(480, 0), P(300, 560), P(120, 0)
};
static const size_t a_ends[] = { 7 };
static const simple_glyph a_glyph = { a_points, 7, a_ends, 1 };

static const font_fixture_point v_points[] = {
    P(0, 700), P(120, 700), P(300, 140), P(480, 700), P(600, 700), P(350, 0), P(250, 0)
};
static const size_t v_ends[] = { 7 };
static const simple_glyph v_glyph = { v_points, 7, v_ends, 1 };

static const font_fixture_point t_points[] = { RECT(0, 600, 600, 700), RECT(250, 0, 350, 700) };
static const size_t t_ends[] = { 4, 8 };
static const simple_glyph t_glyph = { t_points, 8, t_ends, 2 };

static const font_fixture_point o_small_points[] = {
    P(250, 500), Q(500, 500), P(500, 250), Q(500, 0), P(250, 0), Q(0, 0), P(0, 250), Q(0, 500),
    P(380, 250), Q(380, 380), P(250, 380), Q(120, 380), P(120, 250), Q(120, 120), P(250, 120), Q(380, 120)
};
static const size_t o_small_ends[] = { 8, 16 };
static const simple_glyph o_small_glyph = { o_small_points, 16, o_small_ends, 2 };

static const font_fixture_point a_small_points[] = { RECT(50, 0, 450, 500), HOLE(150, 100, 350, 400) };
static const size_t a_small_ends[] = { 4, 8 };
static const simple_glyph a_small_glyph = { a_small_points, 8, a_small_ends, 2 };

static const font_fixture_point dieresis_points[] = { RECT(100, 800, 200, 900), RECT(400, 800, 500, 900) };
static const size_t dieresis_ends[] = { 4, 8 };
static const simple_glyph dieresis_glyph = { dieresis_points, 8, dieresis_ends, 2 };

static const font_fixture_point macron_points[] = { RECT(100, 1000, 500, 1060) };
static const size_t macron_ends[] = { 4 };
static const simple_glyph macron_glyph = { macron_points, 4, macron_ends, 1 };

static const font_fixture_point emoji_points[] = { RECT(100, 100, 400, 400) };
static const size_t emoji_ends[] = { 4 };
static const simple_glyph emoji_glyph = { emoji_points, 4, emoji_ends, 1 };

static const font_fixture_point ring_points[] = { RECT(0, 0, 100, 100), HOLE(25, 25, 75, 75) };
static const size_t ring_ends[] = { 4, 8 };
static const simple_glyph ring_glyph = { ring_points, 8, ring_ends, 2 };

/* Ä takes its advance from A through USE_MY_METRICS; its own hmtx entry says something else. */
static const component a_dieresis_components[] = {
    { GID_A, ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES | USE_MY_METRICS, 0, 0, 1, 0, 0, 1 },
    { GID_DIERESIS, ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES, 0, -20, 1, 0, 0, 1 }
};
/* ª: one scaled component behind byte arguments. */
static const component ordinal_components[] = {
    { GID_A_SMALL, ARGS_ARE_XY_VALUES | WE_HAVE_A_SCALE, 50, 100, 0.5, 0, 0, 0.5 }
};
/* Ɐ: A turned half a revolution by a two-by-two matrix. */
static const component turned_a_components[] = {
    { GID_A, ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES | WE_HAVE_A_TWO_BY_TWO, 600, 700, -1, 0, 0, -1 }
};
/* Ǟ: a composite inside a composite, the macron overlapping the dots it was moved onto. */
static const component a_dieresis_macron_components[] = {
    { GID_A_DIERESIS, ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES, 0, 0, 1, 0, 0, 1 },
    { GID_MACRON, ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES, 0, -200, 1, 0, 0, 1 }
};
/* Å: the ring placed by point matching, its point 0 onto point 2 of the A before it. */
static const component a_ring_components[] = {
    { GID_A, ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES | USE_MY_METRICS, 0, 0, 1, 0, 0, 1 },
    { GID_RING, 0, 2, 0, 1, 0, 0, 1 }
};

static const glyph_def glyphs[GID_COUNT] = {
    [GID_NOTDEF] = { 500, NULL, NULL, 0 },
    [GID_SPACE] = { 250, NULL, NULL, 0 },
    [GID_H] = { 700, &h_glyph, NULL, 0 },
    [GID_TSE] = { 720, &tse_glyph, NULL, 0 },
    [GID_O] = { 700, &o_glyph, NULL, 0 },
    [GID_A] = { 600, &a_glyph, NULL, 0 },
    [GID_V] = { 600, &v_glyph, NULL, 0 },
    [GID_T] = { 600, &t_glyph, NULL, 0 },
    [GID_O_SMALL] = { 500, &o_small_glyph, NULL, 0 },
    [GID_A_SMALL] = { 500, &a_small_glyph, NULL, 0 },
    [GID_DIERESIS] = { 600, &dieresis_glyph, NULL, 0 },
    [GID_A_DIERESIS] = { 999, NULL, a_dieresis_components, 2 },
    [GID_ORDINAL] = { 300, NULL, ordinal_components, 1 },
    [GID_TURNED_A] = { 600, NULL, turned_a_components, 1 },
    [GID_A_DIERESIS_MACRON] = { 600, NULL, a_dieresis_macron_components, 2 },
    [GID_MACRON] = { 600, &macron_glyph, NULL, 0 },
    [GID_EMOJI] = { 500, &emoji_glyph, NULL, 0 },
    [GID_A_RING] = { 999, NULL, a_ring_components, 2 },
    [GID_RING] = { 100, &ring_glyph, NULL, 0 }
};

typedef struct mapping {
    uint32_t code;
    uint16_t glyph;
} mapping;

/* Ascending by code. U+00A0 shares the space's glyph; U+1F600 is reachable only through format 12. */
static const mapping mappings[] = {
    { 0x0020, GID_SPACE }, { 0x0041, GID_A }, { 0x0048, GID_H }, { 0x004F, GID_O }, { 0x0054, GID_T },
    { 0x0056, GID_V }, { 0x0061, GID_A_SMALL }, { 0x006F, GID_O_SMALL }, { 0x00A0, GID_SPACE },
    { 0x00A8, GID_DIERESIS }, { 0x00AA, GID_ORDINAL }, { 0x00AF, GID_MACRON }, { 0x00C4, GID_A_DIERESIS },
    { 0x00C5, GID_A_RING }, { 0x01DE, GID_A_DIERESIS_MACRON }, { 0x0426, GID_TSE },
    { 0x2C6F, GID_TURNED_A }, { 0x1F600, GID_EMOJI }
};
#define MAPPING_COUNT (sizeof mappings / sizeof mappings[0])

static const uint32_t gpos_codes[] = {
    0x0020, 0x0041, 0x0048, 0x004F, 0x0054, 0x0056, 0x0061, 0x006F, 0x00A0, 0x00A8, 0x00AA, 0x00AF,
    0x00C4, 0x00C5, 0x01DE, 0x0426, 0x2C6F, 0x1F600
};
static const uint32_t kern_codes[] = {
    0x0020, 0x0041, 0x0048, 0x004F, 0x0054, 0x0056, 0x0061, 0x006F, 0x00A0, 0x00A8, 0x00AA, 0x00AF,
    0x00C4, 0x00C5, 0x01DE, 0x0426, 0x2C6F
};

static const font_fixture_pair gpos_pairs[] = {
    { 0x0041, 0x004F, 0 }, { 0x0041, 0x0056, -80 }, { 0x0041, 0x1F600, -50 }, { 0x0048, 0x0048, 3 },
    { 0x0054, 0x0041, -60 }, { 0x0054, 0x0061, -120 }, { 0x0054, 0x006F, -120 }, { 0x0056, 0x0041, -80 }
};
static const font_fixture_pair kern_pairs[] = {
    { 0x0041, 0x0056, -80 }, { 0x0054, 0x006F, -120 }, { 0x0056, 0x0041, -80 }
};

/* ------------------------------------------------------------------------------------------------
 * A growable big-endian byte buffer, the only writer every table goes through.
 * ---------------------------------------------------------------------------------------------- */

typedef struct buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} buffer;

static void put_bytes(buffer *out, const void *bytes, size_t size) {
    if (out->failed) {
        return;
    }
    if (out->size + size > out->capacity) {
        size_t capacity = out->capacity == 0 ? 256u : out->capacity;
        uint8_t *grown;
        while (capacity < out->size + size) {
            capacity *= 2u;
        }
        grown = realloc(out->data, capacity);
        if (grown == NULL) {
            out->failed = 1;
            return;
        }
        out->data = grown;
        out->capacity = capacity;
    }
    if (size != 0) {
        memcpy(out->data + out->size, bytes, size);
    }
    out->size += size;
}

static void put8(buffer *out, unsigned value) {
    const uint8_t byte = (uint8_t)value;
    put_bytes(out, &byte, 1);
}

static void put16(buffer *out, int value) {
    const uint8_t bytes[2] = { (uint8_t)((unsigned)value >> 8), (uint8_t)value };
    put_bytes(out, bytes, 2);
}

static void put32(buffer *out, uint32_t value) {
    const uint8_t bytes[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value
    };
    put_bytes(out, bytes, 4);
}

static void put_tag(buffer *out, const char tag[4]) {
    put_bytes(out, tag, 4);
}

static void put_buffer(buffer *out, const buffer *part) {
    if (part->failed) {
        out->failed = 1;
    }
    put_bytes(out, part->data, part->size);
}

static void pad_to(buffer *out, size_t alignment) {
    while (out->size % alignment != 0) {
        put8(out, 0);
    }
}

static void patch16(buffer *out, size_t at, size_t value) {
    if (out->failed || at + 2u > out->size || value > 0xFFFFu) {
        out->failed = 1;
        return;
    }
    out->data[at] = (uint8_t)(value >> 8);
    out->data[at + 1u] = (uint8_t)value;
}

static void patch32(buffer *out, size_t at, uint32_t value) {
    if (out->failed || at + 4u > out->size) {
        out->failed = 1;
        return;
    }
    out->data[at] = (uint8_t)(value >> 24);
    out->data[at + 1u] = (uint8_t)(value >> 16);
    out->data[at + 2u] = (uint8_t)(value >> 8);
    out->data[at + 3u] = (uint8_t)value;
}

static void release(buffer *out) {
    free(out->data);
    memset(out, 0, sizeof *out);
}

/* ------------------------------------------------------------------------------------------------
 * Composition: the truth an independent rasterizer receives, and the bounds `glyf` records.
 * ---------------------------------------------------------------------------------------------- */

typedef struct points {
    font_fixture_point *items;
    size_t count;
    size_t capacity;
    size_t *ends;
    size_t contour_count;
    size_t contour_capacity;
    int failed;
} points;

static void add_point(points *out, font_fixture_point point) {
    if (out->failed) {
        return;
    }
    if (out->count == out->capacity) {
        const size_t capacity = out->capacity == 0 ? 32u : out->capacity * 2u;
        font_fixture_point *grown = realloc(out->items, capacity * sizeof *grown);
        if (grown == NULL) {
            out->failed = 1;
            return;
        }
        out->items = grown;
        out->capacity = capacity;
    }
    out->items[out->count++] = point;
}

static void add_end(points *out, size_t end) {
    if (out->failed) {
        return;
    }
    if (out->contour_count == out->contour_capacity) {
        const size_t capacity = out->contour_capacity == 0 ? 8u : out->contour_capacity * 2u;
        size_t *grown = realloc(out->ends, capacity * sizeof *grown);
        if (grown == NULL) {
            out->failed = 1;
            return;
        }
        out->ends = grown;
        out->contour_capacity = capacity;
    }
    out->ends[out->contour_count++] = end;
}

static void end_contour(points *out) {
    add_end(out, out->count);
}

/* Returns the advance after USE_MY_METRICS, which the hmtx entry of a composite does not have. */
static int compose(uint16_t glyph, points *out) {
    const glyph_def *definition = &glyphs[glyph];
    int advance = definition->advance;
    if (definition->simple != NULL) {
        size_t at = 0;
        for (size_t contour = 0; contour < definition->simple->contour_count; ++contour) {
            for (; at < definition->simple->ends[contour]; ++at) {
                add_point(out, definition->simple->points[at]);
            }
            end_contour(out);
        }
        return advance;
    }
    for (size_t at = 0; at < definition->component_count; ++at) {
        const component *part = &definition->components[at];
        points child = { 0 };
        const int child_advance = compose(part->glyph, &child);
        double dx = part->arg1;
        double dy = part->arg2;
        if (child.failed) {
            out->failed = 1;
        }
        if ((part->flags & ARGS_ARE_XY_VALUES) == 0) {
            const font_fixture_point parent = out->items[part->arg1];
            const font_fixture_point own = child.items[part->arg2];
            dx = parent.x - (part->a * own.x + part->c * own.y);
            dy = parent.y - (part->b * own.x + part->d * own.y);
        }
        {
            const size_t base = out->count;
            for (size_t point = 0; point < child.count; ++point) {
                const font_fixture_point source = child.items[point];
                font_fixture_point moved;
                moved.x = part->a * source.x + part->c * source.y + dx;
                moved.y = part->b * source.x + part->d * source.y + dy;
                moved.on_curve = source.on_curve;
                add_point(out, moved);
            }
            for (size_t contour = 0; contour < child.contour_count; ++contour) {
                add_end(out, base + child.ends[contour]);
            }
        }
        if ((part->flags & USE_MY_METRICS) != 0) {
            advance = child_advance;
        }
        free(child.items);
        free(child.ends);
    }
    return advance;
}

static void bounds_of(const points *outline, int box[4]) {
    if (outline->count == 0) {
        box[0] = box[1] = box[2] = box[3] = 0;
        return;
    }
    box[0] = box[2] = (int)floor(outline->items[0].x);
    box[1] = box[3] = (int)floor(outline->items[0].y);
    for (size_t at = 0; at < outline->count; ++at) {
        const int x = (int)floor(outline->items[at].x), y = (int)floor(outline->items[at].y);
        const int right = (int)ceil(outline->items[at].x), top = (int)ceil(outline->items[at].y);
        if (x < box[0]) {
            box[0] = x;
        }
        if (y < box[1]) {
            box[1] = y;
        }
        if (right > box[2]) {
            box[2] = right;
        }
        if (top > box[3]) {
            box[3] = top;
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * Tables.
 * ---------------------------------------------------------------------------------------------- */

static void simple_glyph_data(const simple_glyph *glyph, buffer *out) {
    points outline = { 0 };
    int box[4];
    uint8_t flags[64];
    int previous_x = 0, previous_y = 0;
    for (size_t at = 0; at < glyph->point_count; ++at) {
        add_point(&outline, glyph->points[at]);
    }
    bounds_of(&outline, box);
    free(outline.items);
    put16(out, (int)glyph->contour_count);
    for (int at = 0; at < 4; ++at) {
        put16(out, box[at]);
    }
    for (size_t at = 0; at < glyph->contour_count; ++at) {
        put16(out, (int)glyph->ends[at] - 1);
    }
    put16(out, 0);
    for (size_t at = 0; at < glyph->point_count; ++at) {
        const int dx = (int)glyph->points[at].x - previous_x;
        const int dy = (int)glyph->points[at].y - previous_y;
        uint8_t flag = glyph->points[at].on_curve ? 0x01u : 0x00u;
        if (dx == 0) {
            flag |= 0x10u;
        } else if (dx >= -255 && dx <= 255) {
            flag |= (uint8_t)(0x02u | (dx > 0 ? 0x10u : 0u));
        }
        if (dy == 0) {
            flag |= 0x20u;
        } else if (dy >= -255 && dy <= 255) {
            flag |= (uint8_t)(0x04u | (dy > 0 ? 0x20u : 0u));
        }
        flags[at] = flag;
        previous_x = (int)glyph->points[at].x;
        previous_y = (int)glyph->points[at].y;
    }
    /* Runs of one flag use REPEAT, so a reader that ignores it reads the coordinates wrong. */
    for (size_t at = 0; at < glyph->point_count;) {
        size_t run = 1;
        while (at + run < glyph->point_count && flags[at + run] == flags[at] && run < 256u) {
            ++run;
        }
        if (run >= 3u) {
            put8(out, flags[at] | 0x08u);
            put8(out, (unsigned)(run - 1u));
        } else {
            for (size_t copy = 0; copy < run; ++copy) {
                put8(out, flags[at]);
            }
        }
        at += run;
    }
    previous_x = previous_y = 0;
    for (size_t at = 0; at < glyph->point_count; ++at) {
        const int dx = (int)glyph->points[at].x - previous_x;
        if ((flags[at] & 0x02u) != 0) {
            put8(out, (unsigned)abs(dx));
        } else if ((flags[at] & 0x10u) == 0) {
            put16(out, dx);
        }
        previous_x = (int)glyph->points[at].x;
    }
    for (size_t at = 0; at < glyph->point_count; ++at) {
        const int dy = (int)glyph->points[at].y - previous_y;
        if ((flags[at] & 0x04u) != 0) {
            put8(out, (unsigned)abs(dy));
        } else if ((flags[at] & 0x20u) == 0) {
            put16(out, dy);
        }
        previous_y = (int)glyph->points[at].y;
    }
}

static int f2dot14(double value) {
    return (int)lround(value * 16384.0) & 0xFFFF;
}

static void composite_glyph_data(uint16_t glyph, buffer *out) {
    const glyph_def *definition = &glyphs[glyph];
    points outline = { 0 };
    int box[4];
    (void)compose(glyph, &outline);
    bounds_of(&outline, box);
    free(outline.items);
    free(outline.ends);
    put16(out, -1);
    for (int at = 0; at < 4; ++at) {
        put16(out, box[at]);
    }
    for (size_t at = 0; at < definition->component_count; ++at) {
        const component *part = &definition->components[at];
        const uint16_t more = at + 1u < definition->component_count ? MORE_COMPONENTS : 0u;
        put16(out, part->flags | more);
        put16(out, part->glyph);
        if ((part->flags & ARG_1_AND_2_ARE_WORDS) != 0) {
            put16(out, part->arg1);
            put16(out, part->arg2);
        } else {
            put8(out, (unsigned)part->arg1 & 0xFFu);
            put8(out, (unsigned)part->arg2 & 0xFFu);
        }
        if ((part->flags & WE_HAVE_A_SCALE) != 0) {
            put16(out, f2dot14(part->a));
        } else if ((part->flags & WE_HAVE_AN_X_AND_Y_SCALE) != 0) {
            put16(out, f2dot14(part->a));
            put16(out, f2dot14(part->d));
        } else if ((part->flags & WE_HAVE_A_TWO_BY_TWO) != 0) {
            put16(out, f2dot14(part->a));
            put16(out, f2dot14(part->b));
            put16(out, f2dot14(part->c));
            put16(out, f2dot14(part->d));
        }
    }
}

/* What hmtx records as the left side bearing: the xMin `glyf` records, as in any real font. */
static int glyph_x_min(uint16_t glyph) {
    points outline = { 0 };
    int box[4];
    (void)compose(glyph, &outline);
    bounds_of(&outline, box);
    free(outline.items);
    free(outline.ends);
    return box[0];
}

static void glyph_data(uint16_t glyph, buffer *out) {
    if (glyphs[glyph].simple != NULL) {
        simple_glyph_data(glyphs[glyph].simple, out);
    } else if (glyphs[glyph].components != NULL) {
        composite_glyph_data(glyph, out);
    }
}

typedef struct table {
    char tag[5];
    buffer data;
} table;

typedef struct font_builder {
    table tables[16];
    size_t count;
} font_builder;

static buffer *new_table(font_builder *font, const char *tag) {
    table *added = &font->tables[font->count++];
    memcpy(added->tag, tag, 5);
    memset(&added->data, 0, sizeof added->data);
    return &added->data;
}

static void head_table(buffer *out, int long_offsets, const int box[4]) {
    put32(out, 0x00010000u);
    put32(out, 0x00010000u);
    put32(out, 0);
    put32(out, 0x5F0F3CF5u);
    put16(out, 0x000B);
    put16(out, FONT_FIXTURE_UNITS_PER_EM);
    for (int at = 0; at < 4; ++at) {
        put32(out, 0);
    }
    for (int at = 0; at < 4; ++at) {
        put16(out, box[at]);
    }
    put16(out, 0);
    put16(out, 8);
    put16(out, 2);
    put16(out, long_offsets ? 1 : 0);
    put16(out, 0);
}

static void hhea_table(buffer *out, unsigned metrics) {
    put32(out, 0x00010000u);
    put16(out, 900);
    put16(out, -200);
    put16(out, 0);
    put16(out, 1000);
    put16(out, 0);
    put16(out, 0);
    put16(out, 1000);
    put16(out, 1);
    put16(out, 0);
    put16(out, 0);
    for (int at = 0; at < 5; ++at) {
        put16(out, 0);
    }
    put16(out, (int)metrics);
}

static void maxp_table(buffer *out, unsigned glyph_count) {
    put32(out, 0x00010000u);
    put16(out, (int)glyph_count);
    put16(out, 64);
    put16(out, 8);
    put16(out, 64);
    put16(out, 8);
    put16(out, 2);
    for (int at = 0; at < 6; ++at) {
        put16(out, 0);
    }
    put16(out, 2);
    put16(out, 2);
}

static void os2_table(buffer *out, int cap_height) {
    put16(out, cap_height >= 0 ? 4 : 1);
    put16(out, 500);
    put16(out, 400);
    put16(out, 5);
    put16(out, 0);
    for (int at = 0; at < 10; ++at) {
        put16(out, 0);
    }
    put16(out, 0);
    for (int at = 0; at < 10; ++at) {
        put8(out, 0);
    }
    for (int at = 0; at < 4; ++at) {
        put32(out, 0);
    }
    put_tag(out, "NONE");
    put16(out, 0x0040);
    put16(out, 0x0020);
    put16(out, 0xFFFF);
    put16(out, 800);
    put16(out, -200);
    put16(out, 0);
    put16(out, 900);
    put16(out, 200);
    put32(out, 1);
    put32(out, 0);
    if (cap_height >= 0) {
        put16(out, 500);
        put16(out, cap_height);
        put16(out, 0);
        put16(out, 0x20);
        put16(out, 2);
    }
}

static void post_table(buffer *out) {
    put32(out, 0x00030000u);
    put32(out, 0);
    put16(out, -100);
    put16(out, 50);
    for (int at = 0; at < 5; ++at) {
        put32(out, 0);
    }
}

typedef struct name_record {
    int id;
    const char *text;
} name_record;

static void name_table(buffer *out, const name_record *records, size_t count) {
    buffer strings = { 0 };
    put16(out, 0);
    put16(out, (int)count);
    put16(out, (int)(6u + 12u * count));
    for (size_t at = 0; at < count; ++at) {
        const size_t length = strlen(records[at].text);
        put16(out, 3);
        put16(out, 1);
        put16(out, 0x0409);
        put16(out, records[at].id);
        put16(out, (int)(length * 2u));
        put16(out, (int)strings.size);
        for (size_t letter = 0; letter < length; ++letter) {
            put16(&strings, (unsigned char)records[at].text[letter]);
        }
    }
    put_buffer(out, &strings);
    release(&strings);
}

/* Segments of one code each, plus one run kept by idRangeOffset so both lookups are exercised. */
static void cmap_format4(buffer *out) {
    uint32_t starts[32], ends[32];
    int deltas[32];
    int ranged[32] = { 0 };
    size_t segments = 0;
    buffer glyph_ids = { 0 };
    for (size_t at = 0; at < MAPPING_COUNT; ++at) {
        if (mappings[at].code > 0xFFFFu) {
            continue;
        }
        if (mappings[at].code == 0x00A8u) {
            /* U+00A8..U+00AF through glyphIdArray, with unmapped holes inside the run. */
            starts[segments] = 0x00A8u;
            ends[segments] = 0x00AFu;
            deltas[segments] = 0;
            ranged[segments] = 1;
            ++segments;
            continue;
        }
        if (mappings[at].code > 0x00A8u && mappings[at].code <= 0x00AFu) {
            continue;
        }
        starts[segments] = ends[segments] = mappings[at].code;
        deltas[segments] = (int)mappings[at].glyph - (int)mappings[at].code;
        ++segments;
    }
    starts[segments] = ends[segments] = 0xFFFFu;
    deltas[segments] = 1;
    ++segments;
    {
        const size_t header = 14u, arrays = 8u * segments + 2u;
        unsigned search = 1, selector = 0;
        while (search * 2u <= segments) {
            search *= 2u;
            ++selector;
        }
        put16(out, 4);
        put16(out, 0);
        put16(out, 0);
        put16(out, (int)(segments * 2u));
        put16(out, (int)(search * 2u));
        put16(out, (int)selector);
        put16(out, (int)(segments * 2u - search * 2u));
        for (size_t at = 0; at < segments; ++at) {
            put16(out, (int)ends[at]);
        }
        put16(out, 0);
        for (size_t at = 0; at < segments; ++at) {
            put16(out, (int)starts[at]);
        }
        for (size_t at = 0; at < segments; ++at) {
            put16(out, deltas[at]);
        }
        for (size_t at = 0; at < segments; ++at) {
            if (!ranged[at]) {
                put16(out, 0);
                continue;
            }
            /* Bytes from this idRangeOffset entry to the start of its glyph ids. */
            put16(out, (int)(2u * (segments - at) + glyph_ids.size));
            for (uint32_t code = starts[at]; code <= ends[at]; ++code) {
                uint16_t glyph = 0;
                for (size_t look = 0; look < MAPPING_COUNT; ++look) {
                    if (mappings[look].code == code) {
                        glyph = mappings[look].glyph;
                    }
                }
                put16(&glyph_ids, glyph);
            }
        }
        put_buffer(out, &glyph_ids);
        patch16(out, 2, header + arrays + glyph_ids.size);
    }
    release(&glyph_ids);
}

static void cmap_format12(buffer *out, uint32_t first, uint32_t count, uint16_t first_glyph) {
    size_t groups = 0;
    put16(out, 12);
    put16(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    if (count != 0) {
        put32(out, first);
        put32(out, first + count - 1u);
        put32(out, first_glyph);
        groups = 1;
    } else {
        for (size_t at = 0; at < MAPPING_COUNT; ++at) {
            put32(out, mappings[at].code);
            put32(out, mappings[at].code);
            put32(out, mappings[at].glyph);
            ++groups;
        }
    }
    patch32(out, 4, (uint32_t)out->size);
    patch32(out, 12, (uint32_t)groups);
}

static void cmap_table(buffer *out, font_fixture_variant variant) {
    buffer format4 = { 0 }, format12 = { 0 };
    const int crowded = variant == FONT_FIXTURE_CROWDED;
    const int with12 = variant != FONT_FIXTURE_KERN;
    const int with4 = !crowded;
    const unsigned count = (unsigned)with4 + (unsigned)with12;
    size_t offset = 4u + 8u * count;
    if (with4) {
        cmap_format4(&format4);
    }
    if (with12) {
        cmap_format12(&format12, crowded ? FONT_FIXTURE_CROWDED_FIRST : 0u,
            crowded ? FONT_FIXTURE_CROWDED_COUNT : 0u, 1);
    }
    put16(out, 0);
    put16(out, (int)count);
    if (with4) {
        put16(out, 3);
        put16(out, 1);
        put32(out, (uint32_t)offset);
        offset += format4.size;
    }
    if (with12) {
        put16(out, 3);
        put16(out, 10);
        put32(out, (uint32_t)offset);
    }
    put_buffer(out, &format4);
    put_buffer(out, &format12);
    release(&format4);
    release(&format12);
}

/* --- GPOS, built bottom-up: every offset is the size of what precedes it in its parent. --- */

static void coverage_list(buffer *out, const uint16_t *glyph_ids, size_t count) {
    put16(out, 1);
    put16(out, (int)count);
    for (size_t at = 0; at < count; ++at) {
        put16(out, glyph_ids[at]);
    }
}

static void coverage_range(buffer *out, uint16_t first, uint16_t last) {
    put16(out, 2);
    put16(out, 1);
    put16(out, first);
    put16(out, last);
    put16(out, 0);
}

typedef struct pair_value {
    uint16_t second;
    int x_advance;
} pair_value;

/* PairPos format 1 with XAdvance on the first glyph only. */
static void pair_list(buffer *out, const uint16_t *firsts, size_t first_count,
    const pair_value *const *sets, const size_t *set_sizes) {
    buffer coverage = { 0 }, body = { 0 };
    const size_t header = 10u + 2u * first_count;
    size_t at;
    coverage_list(&coverage, firsts, first_count);
    put16(out, 1);
    put16(out, 0);
    put16(out, 0x0004);
    put16(out, 0);
    put16(out, (int)first_count);
    for (at = 0; at < first_count; ++at) {
        put16(out, (int)(header + body.size));
        put16(&body, (int)set_sizes[at]);
        for (size_t pair = 0; pair < set_sizes[at]; ++pair) {
            put16(&body, sets[at][pair].second);
            put16(&body, sets[at][pair].x_advance);
        }
    }
    patch16(out, 2, header + body.size);
    put_buffer(out, &body);
    put_buffer(out, &coverage);
    release(&coverage);
    release(&body);
}

/*
 * PairPos format 2 over {T} × {o, a | A}. The first value record carries XPlacement before its
 * XAdvance and the second glyph has a value of its own, so record sizes and field offsets matter.
 */
static void pair_classes(buffer *out) {
    buffer coverage = { 0 }, first_classes = { 0 }, second_classes = { 0 };
    static const int records[2][3][3] = {
        { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
        { { 0, 0, 0 }, { 7, -120, 5 }, { 0, -60, 0 } }
    };
    const size_t header = 16u + 2u * 3u * 3u * 2u;
    coverage_range(&coverage, GID_T, GID_T);
    put16(&first_classes, 2);
    put16(&first_classes, 1);
    put16(&first_classes, GID_T);
    put16(&first_classes, GID_T);
    put16(&first_classes, 1);
    put16(&second_classes, 1);
    put16(&second_classes, GID_A);
    put16(&second_classes, 5);
    put16(&second_classes, 2);
    put16(&second_classes, 0);
    put16(&second_classes, 0);
    put16(&second_classes, 1);
    put16(&second_classes, 1);
    put16(out, 2);
    put16(out, (int)header);
    put16(out, 0x0005);
    put16(out, 0x0004);
    put16(out, (int)(header + coverage.size));
    put16(out, (int)(header + coverage.size + first_classes.size));
    put16(out, 2);
    put16(out, 3);
    for (int first = 0; first < 2; ++first) {
        for (int second = 0; second < 3; ++second) {
            put16(out, records[first][second][0]);
            put16(out, records[first][second][1]);
            put16(out, records[first][second][2]);
        }
    }
    put_buffer(out, &coverage);
    put_buffer(out, &first_classes);
    put_buffer(out, &second_classes);
    release(&coverage);
    release(&first_classes);
    release(&second_classes);
}

static void extension(buffer *out, const buffer *inner) {
    put16(out, 1);
    put16(out, 2);
    put32(out, 8);
    put_buffer(out, inner);
}

static void lookup(buffer *out, int type, buffer *subtables, size_t count) {
    const size_t header = 6u + 2u * count;
    size_t offset = header;
    put16(out, type);
    put16(out, 0);
    put16(out, (int)count);
    for (size_t at = 0; at < count; ++at) {
        put16(out, (int)offset);
        offset += subtables[at].size;
    }
    for (size_t at = 0; at < count; ++at) {
        put_buffer(out, &subtables[at]);
    }
}

static void offset_list(buffer *out, buffer *items, size_t count) {
    const size_t header = 2u + 2u * count;
    size_t offset = header;
    put16(out, (int)count);
    for (size_t at = 0; at < count; ++at) {
        put16(out, (int)offset);
        offset += items[at].size;
    }
    for (size_t at = 0; at < count; ++at) {
        put_buffer(out, &items[at]);
    }
}

static void lang_sys(buffer *out, const uint16_t *features, size_t count) {
    put16(out, 0);
    put16(out, 0xFFFF);
    put16(out, (int)count);
    for (size_t at = 0; at < count; ++at) {
        put16(out, features[at]);
    }
}

static void gpos_table(buffer *out) {
    /*
     * Features sorted by tag: 0 'dist', 1 'kern' (DFLT, latn, cyrl), 2 'kern' (grek only), 3 'kern'
     * (latn's Turkish only: a language a font may tailor, not what text without one gets).
     */
    static const uint16_t kern_feature[] = { 1 };
    static const uint16_t latin_features[] = { 1, 0 };
    static const uint16_t greek_feature[] = { 2 };
    static const uint16_t turkish_features[] = { 1, 3 };
    static const uint16_t first_glyphs[] = { GID_H, GID_A, GID_V };
    static const pair_value h_set[] = { { GID_H, 3 } };
    static const pair_value a_set[] = { { GID_O, 0 }, { GID_V, -80 }, { GID_EMOJI, -50 } };
    static const pair_value v_set[] = { { GID_A, -80 } };
    static const pair_value *const sets[] = { h_set, a_set, v_set };
    static const size_t set_sizes[] = { 1, 3, 1 };
    static const uint16_t t_first[] = { GID_T };
    static const pair_value t_late[] = { { GID_O_SMALL, -999 } };
    static const pair_value *const late_sets[] = { t_late };
    static const size_t late_sizes[] = { 1 };
    static const uint16_t a_only[] = { GID_A };
    static const pair_value a_greek[] = { { GID_A, -500 } };
    static const pair_value *const greek_sets[] = { a_greek };
    static const uint16_t v_only[] = { GID_V };
    static const pair_value v_distance[] = { { GID_V, -300 } };
    static const pair_value *const distance_sets[] = { v_distance };
    static const uint16_t o_only[] = { GID_O };
    static const pair_value o_turkish[] = { { GID_O, -400 } };
    static const pair_value *const turkish_sets[] = { o_turkish };
    static const size_t one[] = { 1 };
    buffer scripts[4] = { { 0 } }, features[4] = { { 0 } }, lookups[5] = { { 0 } };
    buffer script_list = { 0 }, feature_list = { 0 }, lookup_list = { 0 };
    buffer subtables[2] = { { 0 } }, inner = { 0 };
    static const char *const script_tags[] = { "DFLT", "cyrl", "grek", "latn" };
    static const char *const feature_tags[] = { "dist", "kern", "kern", "kern" };

    /* DFLT and cyrl: the kern feature only; grek: the other kern feature; latn: kern, dist, TRK. */
    for (int at = 0; at < 4; ++at) {
        buffer default_lang = { 0 }, turkish = { 0 };
        if (at == 2) {
            lang_sys(&default_lang, greek_feature, 1);
        } else if (at == 3) {
            lang_sys(&default_lang, latin_features, 2);
        } else {
            lang_sys(&default_lang, kern_feature, 1);
        }
        if (at == 3) {
            lang_sys(&turkish, turkish_features, 2);
            put16(&scripts[at], 10);
            put16(&scripts[at], 1);
            put_tag(&scripts[at], "TRK ");
            put16(&scripts[at], (int)(10u + default_lang.size));
        } else {
            put16(&scripts[at], 4);
            put16(&scripts[at], 0);
        }
        put_buffer(&scripts[at], &default_lang);
        put_buffer(&scripts[at], &turkish);
        release(&default_lang);
        release(&turkish);
    }
    {
        size_t offset = 2u + 6u * 4u;
        put16(&script_list, 4);
        for (int at = 0; at < 4; ++at) {
            put_tag(&script_list, script_tags[at]);
            put16(&script_list, (int)offset);
            offset += scripts[at].size;
        }
        for (int at = 0; at < 4; ++at) {
            put_buffer(&script_list, &scripts[at]);
        }
    }
    {
        static const uint16_t feature_lookups[4][2] = { { 3, 0 }, { 0, 1 }, { 2, 0 }, { 4, 0 } };
        static const size_t feature_sizes[4] = { 1, 2, 1, 1 };
        size_t offset = 2u + 6u * 4u;
        for (int at = 0; at < 4; ++at) {
            put16(&features[at], 0);
            put16(&features[at], (int)feature_sizes[at]);
            for (size_t index = 0; index < feature_sizes[at]; ++index) {
                put16(&features[at], feature_lookups[at][index]);
            }
        }
        put16(&feature_list, 4);
        for (int at = 0; at < 4; ++at) {
            put_tag(&feature_list, feature_tags[at]);
            put16(&feature_list, (int)offset);
            offset += features[at].size;
        }
        for (int at = 0; at < 4; ++at) {
            put_buffer(&feature_list, &features[at]);
        }
    }

    pair_list(&subtables[0], first_glyphs, 3, sets, set_sizes);
    lookup(&lookups[0], 2, subtables, 1);
    release(&subtables[0]);

    /* Lookup 1: two Extension subtables; the format 2 one matches T first, so the later -999 is dead. */
    pair_classes(&inner);
    extension(&subtables[0], &inner);
    release(&inner);
    pair_list(&inner, t_first, 1, late_sets, late_sizes);
    extension(&subtables[1], &inner);
    release(&inner);
    lookup(&lookups[1], 9, subtables, 2);
    release(&subtables[0]);
    release(&subtables[1]);

    pair_list(&subtables[0], a_only, 1, greek_sets, one);
    lookup(&lookups[2], 2, subtables, 1);
    release(&subtables[0]);
    pair_list(&subtables[0], v_only, 1, distance_sets, one);
    lookup(&lookups[3], 2, subtables, 1);
    release(&subtables[0]);
    pair_list(&subtables[0], o_only, 1, turkish_sets, one);
    lookup(&lookups[4], 2, subtables, 1);
    release(&subtables[0]);
    offset_list(&lookup_list, lookups, 5);

    put16(out, 1);
    put16(out, 0);
    put16(out, 10);
    put16(out, (int)(10u + script_list.size));
    put16(out, (int)(10u + script_list.size + feature_list.size));
    put_buffer(out, &script_list);
    put_buffer(out, &feature_list);
    put_buffer(out, &lookup_list);
    for (int at = 0; at < 4; ++at) {
        release(&scripts[at]);
        release(&features[at]);
    }
    for (int at = 0; at < 5; ++at) {
        release(&lookups[at]);
    }
    release(&script_list);
    release(&feature_list);
    release(&lookup_list);
}

static void kern_subtable(buffer *out, int coverage, const uint16_t (*pairs)[2], const int *values, size_t count) {
    unsigned search = 1, selector = 0;
    while (search * 2u <= count) {
        search *= 2u;
        ++selector;
    }
    put16(out, 0);
    put16(out, (int)(14u + 6u * count));
    put16(out, coverage);
    put16(out, (int)count);
    put16(out, (int)(search * 6u));
    put16(out, (int)selector);
    put16(out, (int)((count - search) * 6u));
    for (size_t at = 0; at < count; ++at) {
        put16(out, pairs[at][0]);
        put16(out, pairs[at][1]);
        put16(out, values[at]);
    }
}

static void kern_table(buffer *out) {
    static const uint16_t pairs[3][2] = { { GID_A, GID_V }, { GID_V, GID_A }, { GID_T, GID_O_SMALL } };
    static const int values[3] = { -80, -80, -120 };
    static const uint16_t minimum_pairs[1][2] = { { GID_A, GID_A } };
    static const int minimum_values[1] = { -400 };
    put16(out, 0);
    put16(out, 2);
    kern_subtable(out, 0x0001, pairs, values, 3);
    /* A minimum-value subtable is not kerning; a reader that adds it shortens A A. */
    kern_subtable(out, 0x0003, minimum_pairs, minimum_values, 1);
}

static uint32_t checksum(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    for (size_t at = 0; at < size; at += 4u) {
        uint32_t word = 0;
        for (size_t byte = 0; byte < 4u; ++byte) {
            word = (word << 8) | (at + byte < size ? data[at + byte] : 0u);
        }
        sum += word;
    }
    return sum;
}

static int compare_tables(const void *left, const void *right) {
    return memcmp(((const table *)left)->tag, ((const table *)right)->tag, 4);
}

static test_bytes assemble(font_builder *font, uint32_t version) {
    buffer out = { 0 };
    test_bytes result = { NULL, 0 };
    size_t offset = 12u + 16u * font->count;
    size_t head_at = 0;
    unsigned search = 1, selector = 0;
    int failed = 0;
    qsort(font->tables, font->count, sizeof font->tables[0], compare_tables);
    while (search * 2u <= font->count) {
        search *= 2u;
        ++selector;
    }
    put32(&out, version);
    put16(&out, (int)font->count);
    put16(&out, (int)(search * 16u));
    put16(&out, (int)selector);
    put16(&out, (int)(font->count * 16u - search * 16u));
    for (size_t at = 0; at < font->count; ++at) {
        buffer *data = &font->tables[at].data;
        failed |= data->failed;
        put_tag(&out, font->tables[at].tag);
        put32(&out, checksum(data->data, data->size));
        put32(&out, (uint32_t)offset);
        put32(&out, (uint32_t)data->size);
        if (memcmp(font->tables[at].tag, "head", 4) == 0) {
            head_at = offset;
        }
        offset += (data->size + 3u) & ~(size_t)3u;
    }
    for (size_t at = 0; at < font->count; ++at) {
        put_buffer(&out, &font->tables[at].data);
        pad_to(&out, 4);
        release(&font->tables[at].data);
    }
    if (head_at != 0) {
        patch32(&out, head_at + 8u, 0xB1B0AFBAu - checksum(out.data, out.size));
    }
    if (failed || out.failed) {
        release(&out);
        return result;
    }
    result.data = out.data;
    result.size = out.size;
    return result;
}

static test_bytes truetype(font_fixture_variant variant) {
    font_builder font = { 0 };
    buffer glyf = { 0 }, loca = { 0 };
    const int crowded = variant == FONT_FIXTURE_CROWDED;
    const int long_offsets = variant != FONT_FIXTURE_KERN;
    const unsigned glyph_count = crowded ? FONT_FIXTURE_CROWDED_COUNT + 1u : GID_COUNT;
    const unsigned metrics = variant == FONT_FIXTURE_KERN ? GID_EMOJI + 1u : (crowded ? 2u : GID_COUNT);
    const int box[4] = { -200, -200, 1100, 1100 };
    buffer *table;

    if (crowded) {
        static const font_fixture_point em_points[] = { RECT(0, 0, 1000, 1000) };
        static const size_t em_ends[] = { 4 };
        static const simple_glyph em_glyph = { em_points, 4, em_ends, 1 };
        buffer em = { 0 };
        simple_glyph_data(&em_glyph, &em);
        pad_to(&em, 4);
        /* An empty .notdef, then the em square once per glyph: loca cannot share one range. */
        put32(&loca, 0);
        for (unsigned at = 1; at <= FONT_FIXTURE_CROWDED_COUNT; ++at) {
            put32(&loca, (uint32_t)glyf.size);
            put_buffer(&glyf, &em);
        }
        put32(&loca, (uint32_t)glyf.size);
        release(&em);
    } else {
        for (uint16_t glyph = 0; glyph < GID_COUNT; ++glyph) {
            if (long_offsets) {
                put32(&loca, (uint32_t)glyf.size);
            } else {
                put16(&loca, (int)(glyf.size / 2u));
            }
            glyph_data(glyph, &glyf);
            pad_to(&glyf, long_offsets ? 4u : 2u);
        }
        if (long_offsets) {
            put32(&loca, (uint32_t)glyf.size);
        } else {
            put16(&loca, (int)(glyf.size / 2u));
        }
    }

    table = new_table(&font, "head");
    head_table(table, long_offsets, box);
    table = new_table(&font, "hhea");
    hhea_table(table, metrics);
    table = new_table(&font, "maxp");
    maxp_table(table, glyph_count);
    table = new_table(&font, "OS/2");
    os2_table(table, font_fixture_cap_height(variant));
    table = new_table(&font, "hmtx");
    if (crowded) {
        put16(table, 500);
        put16(table, 0);
        put16(table, 1000);
        put16(table, 0);
        for (unsigned at = 2; at < glyph_count; ++at) {
            put16(table, 0);
        }
    } else {
        for (unsigned glyph = 0; glyph < GID_COUNT; ++glyph) {
            if (glyph < metrics) {
                put16(table, glyphs[glyph].advance);
            }
            put16(table, glyph_x_min((uint16_t)glyph));
        }
    }
    table = new_table(&font, "cmap");
    cmap_table(table, variant);
    table = new_table(&font, "loca");
    put_buffer(table, &loca);
    table = new_table(&font, "glyf");
    put_buffer(table, &glyf);
    table = new_table(&font, "post");
    post_table(table);
    table = new_table(&font, "name");
    if (variant == FONT_FIXTURE_KERN) {
        static const name_record records[] = {
            { 1, "Fixture Medium" }, { 2, "Regular" }, { 16, "Fixture" }, { 17, "Medium" }
        };
        name_table(table, records, 4);
    } else {
        static const name_record records[] = { { 1, "Fixture Sans" }, { 2, "Regular" } };
        name_table(table, records, 2);
    }
    if (variant == FONT_FIXTURE_GPOS || variant == FONT_FIXTURE_VARIABLE) {
        table = new_table(&font, "GPOS");
        gpos_table(table);
    }
    if (variant == FONT_FIXTURE_KERN) {
        table = new_table(&font, "kern");
        kern_table(table);
    }
    if (variant == FONT_FIXTURE_VARIABLE) {
        table = new_table(&font, "fvar");
        put16(table, 1);
        put16(table, 0);
        put16(table, 16);
        put16(table, 2);
        put16(table, 1);
        put16(table, 20);
        put16(table, 0);
        put16(table, 8);
        put_tag(table, "wght");
        put32(table, 100u << 16);
        put32(table, 400u << 16);
        put32(table, 900u << 16);
        put16(table, 0);
        put16(table, 256);
    }
    release(&glyf);
    release(&loca);
    return assemble(&font, 0x00010000u);
}

test_bytes font_fixture(font_fixture_variant variant) {
    if (variant == FONT_FIXTURE_CFF) {
        font_builder font = { 0 };
        buffer *table = new_table(&font, "CFF ");
        put8(table, 1);
        put8(table, 0);
        put8(table, 4);
        put8(table, 1);
        table = new_table(&font, "head");
        {
            const int box[4] = { 0, 0, 1000, 1000 };
            head_table(table, 0, box);
        }
        return assemble(&font, 0x4F54544Fu);
    }
    return truetype(variant);
}

size_t font_fixture_codes(font_fixture_variant variant, const uint32_t **codes) {
    switch (variant) {
        case FONT_FIXTURE_GPOS:
        case FONT_FIXTURE_VARIABLE:
            *codes = gpos_codes;
            return sizeof gpos_codes / sizeof gpos_codes[0];
        case FONT_FIXTURE_KERN:
            *codes = kern_codes;
            return sizeof kern_codes / sizeof kern_codes[0];
        default:
            *codes = NULL;
            return 0;
    }
}

int font_fixture_outline_of(font_fixture_variant variant, uint32_t code, font_fixture_outline *outline) {
    const uint32_t *codes = NULL;
    const size_t count = font_fixture_codes(variant, &codes);
    points composed = { 0 };
    uint16_t glyph = 0;
    int known = 0;
    memset(outline, 0, sizeof *outline);
    for (size_t at = 0; at < count; ++at) {
        known |= codes[at] == code;
    }
    for (size_t at = 0; at < MAPPING_COUNT; ++at) {
        if (mappings[at].code == code) {
            glyph = mappings[at].glyph;
        }
    }
    if (!known || glyph == 0) {
        return 0;
    }
    outline->advance = compose(glyph, &composed);
    if (composed.failed) {
        free(composed.items);
        free(composed.ends);
        return 0;
    }
    /* The KERN variant shares the last full advance among glyphs past numberOfHMetrics. */
    if (variant == FONT_FIXTURE_KERN && glyph > GID_EMOJI && glyphs[glyph].simple != NULL) {
        outline->advance = glyphs[GID_EMOJI].advance;
    }
    outline->points = composed.items;
    outline->point_count = composed.count;
    outline->contour_ends = composed.ends;
    outline->contour_count = composed.contour_count;
    return 1;
}

void font_fixture_outline_free(font_fixture_outline *outline) {
    free(outline->points);
    free(outline->contour_ends);
    memset(outline, 0, sizeof *outline);
}

int font_fixture_cap_height(font_fixture_variant variant) {
    return variant == FONT_FIXTURE_KERN ? -1 : 650;
}

size_t font_fixture_pairs(font_fixture_variant variant, const font_fixture_pair **pairs) {
    switch (variant) {
        case FONT_FIXTURE_GPOS:
        case FONT_FIXTURE_VARIABLE:
            *pairs = gpos_pairs;
            return sizeof gpos_pairs / sizeof gpos_pairs[0];
        case FONT_FIXTURE_KERN:
            *pairs = kern_pairs;
            return sizeof kern_pairs / sizeof kern_pairs[0];
        default:
            *pairs = NULL;
            return 0;
    }
}

font_fixture_frame font_fixture_missing_box(void) {
    const font_fixture_frame frame = { 0.06, -0.06, 0.66, 0.07, 0.78 };
    return frame;
}
