#include "font_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * The TrueType reader. Every offset and length is checked against the bytes it claims before it is
 * followed, and every count against a hard limit before anything is allocated from it.
 */

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

enum {
    ARG_1_AND_2_ARE_WORDS = 0x0001,
    ARGS_ARE_XY_VALUES = 0x0002,
    WE_HAVE_A_SCALE = 0x0008,
    MORE_COMPONENTS = 0x0020,
    WE_HAVE_AN_X_AND_Y_SCALE = 0x0040,
    WE_HAVE_A_TWO_BY_TWO = 0x0080,
    USE_MY_METRICS = 0x0200,
    SCALED_COMPONENT_OFFSET = 0x0800,
    UNSCALED_COMPONENT_OFFSET = 0x1000
};

static uint16_t u16(const uint8_t *at) {
    return (uint16_t)(((unsigned)at[0] << 8) | at[1]);
}

static int16_t s16(const uint8_t *at) {
    return (int16_t)u16(at);
}

static uint32_t u32(const uint8_t *at) {
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

/** Whether `length` bytes from `offset` lie inside `size` bytes, without overflowing. */
static int inside(uint64_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

static edds_status malformed(edds_error *error, const char *what) {
    font_fail(error, "malformed-font", "The TrueType font is malformed: %s.", what);
    return EDDS_INVALID_INPUT;
}

/* --- Character map ---------------------------------------------------------------------------- */

static int valid_format4(const uint8_t *subtable, uint32_t limit) {
    uint32_t segments;
    if (limit < 16u) return 0;
    segments = u16(subtable + 6) / 2u;
    if (segments == 0 || (u16(subtable + 6) & 1u) != 0) return 0;
    return 16u + 8u * segments <= limit;
}

static int valid_format12(const uint8_t *subtable, uint32_t limit) {
    uint64_t groups;
    uint32_t previous_end = 0;
    if (limit < 16u) return 0;
    groups = u32(subtable + 12);
    if (16u + 12u * groups > limit) return 0;
    for (uint64_t at = 0; at < groups; ++at) {
        const uint8_t *group = subtable + 16u + 12u * at;
        const uint32_t first = u32(group), last = u32(group + 4);
        /* Sorted and disjoint, so a lookup may bisect them. */
        if (first > last || last > 0x10FFFFu || (at != 0 && first <= previous_end)) return 0;
        previous_end = last;
    }
    return 1;
}

/** Windows full repertoire, then Unicode full repertoire, then the BMP maps. */
static unsigned cmap_rank(uint16_t platform, uint16_t encoding, uint16_t format) {
    if (format == 12u && platform == 3u && encoding == 10u) return 5;
    if (format == 12u && platform == 0u && (encoding == 4u || encoding == 6u)) return 4;
    if (format == 4u && platform == 3u && encoding == 1u) return 3;
    if (format == 4u && platform == 0u && encoding == 3u) return 2;
    if (format == 4u && platform == 0u) return 1;
    return 0;
}

static edds_status choose_cmap(font_face *face, edds_error *error) {
    const uint8_t *table = face->data + face->cmap.offset;
    const uint32_t length = face->cmap.length;
    unsigned best = 0;
    uint32_t count;
    if (length < 4u) return malformed(error, "the cmap table is truncated");
    count = u16(table + 2);
    if (4u + 8u * count > length) return malformed(error, "the cmap records run past the table");
    for (uint32_t at = 0; at < count; ++at) {
        const uint8_t *record = table + 4u + 8u * at;
        const uint32_t offset = u32(record + 4);
        uint32_t limit;
        unsigned rank;
        if (offset > length - 2u) continue;
        limit = length - offset;
        rank = cmap_rank(u16(record), u16(record + 2), u16(table + offset));
        if (rank <= best) continue;
        if (u16(table + offset) == 4u ? !valid_format4(table + offset, limit) : !valid_format12(table + offset, limit)) {
            return malformed(error, "a character map subtable is truncated or unsorted");
        }
        best = rank;
        face->cmap_offset = face->cmap.offset + offset;
        face->cmap_limit = limit;
        face->cmap_format = u16(table + offset);
    }
    if (best == 0) {
        font_fail(error, "unsupported-cmap", "The font has no Unicode character map of format 4 or 12.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    return EDDS_OK;
}

uint32_t font_face_glyph(const font_face *face, uint32_t code) {
    const uint8_t *subtable = face->data + face->cmap_offset;
    uint32_t glyph = 0;
    if (face->cmap_format == 4u) {
        const uint32_t segments = u16(subtable + 6) / 2u;
        const uint8_t *ends = subtable + 14;
        const uint8_t *starts = ends + 2u * segments + 2u;
        const uint8_t *deltas = starts + 2u * segments;
        const uint8_t *ranges = deltas + 2u * segments;
        if (code > 0xFFFFu) return 0;
        for (uint32_t at = 0; at < segments; ++at) {
            const uint32_t start = u16(starts + 2u * at);
            if (u16(ends + 2u * at) < code) continue;
            if (code < start) return 0;
            if (u16(ranges + 2u * at) == 0) {
                glyph = (code + u16(deltas + 2u * at)) & 0xFFFFu;
            } else {
                const uint64_t address = (uint64_t)(ranges + 2u * at - subtable) + u16(ranges + 2u * at) +
                    2u * (uint64_t)(code - start);
                if (address + 2u > face->cmap_limit) return 0;
                glyph = u16(subtable + address);
                if (glyph != 0) glyph = (glyph + u16(deltas + 2u * at)) & 0xFFFFu;
            }
            break;
        }
    } else {
        uint32_t low = 0, high = u32(subtable + 12);
        while (low < high) {
            const uint32_t middle = low + (high - low) / 2u;
            const uint8_t *group = subtable + 16u + 12u * middle;
            if (code < u32(group)) high = middle;
            else if (code > u32(group + 4)) low = middle + 1u;
            else {
                const uint64_t found = (uint64_t)u32(group + 8) + (code - u32(group));
                glyph = found > UINT32_MAX ? 0u : (uint32_t)found;
                break;
            }
        }
    }
    return glyph < face->glyph_count ? glyph : 0u;
}

uint32_t font_face_advance(const font_face *face, uint32_t glyph) {
    const uint8_t *metrics = face->data + face->hmtx.offset;
    const uint32_t index = glyph < face->metric_count ? glyph : face->metric_count - 1u;
    return u16(metrics + 4u * index);
}

/* --- Opening ---------------------------------------------------------------------------------- */

edds_status font_face_open(font_face *face, const uint8_t *data, size_t size, edds_error *error) {
    uint32_t version;
    uint32_t tables;
    int cff = 0;
    int variable = 0;
    if (face == NULL || data == NULL) {
        font_fail(error, "invalid-api-argument", "The font bytes are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(face, 0, sizeof *face);
    face->data = data;
    face->size = size;
    face->cap_height = -1;
    if (size > FONT_MAX_FILE_BYTES) {
        font_fail(error, "font-size-limit", "A font file is at most %u bytes.", FONT_MAX_FILE_BYTES);
        return EDDS_INVALID_INPUT;
    }
    if (size < 12u) {
        font_fail(error, "not-a-truetype-font", "The file is too short to be a TrueType font.");
        return EDDS_INVALID_INPUT;
    }
    version = u32(data);
    if (version == TAG('O', 'T', 'T', 'O')) {
        font_fail(error, "unsupported-outline-format", "The font has CFF outlines; only TrueType outlines are supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (version == TAG('t', 't', 'c', 'f')) {
        font_fail(error, "font-collection-unsupported", "A font collection holds several fonts; pass one TrueType font.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (version != 0x00010000u && version != TAG('t', 'r', 'u', 'e')) {
        font_fail(error, "not-a-truetype-font", "The file does not start like a TrueType font.");
        return EDDS_INVALID_INPUT;
    }
    tables = u16(data + 4);
    if (!inside(size, 12u, 16u * (uint64_t)tables)) return malformed(error, "the table directory runs past the file");
    for (uint32_t at = 0; at < tables; ++at) {
        const uint8_t *record = data + 12u + 16u * at;
        const uint32_t tag = u32(record);
        font_table *slot = NULL;
        if (!inside(size, u32(record + 8), u32(record + 12))) return malformed(error, "a table lies outside the file");
        switch (tag) {
            case TAG('h', 'e', 'a', 'd'): slot = &face->head; break;
            case TAG('h', 'h', 'e', 'a'): slot = &face->hhea; break;
            case TAG('h', 'm', 't', 'x'): slot = &face->hmtx; break;
            case TAG('m', 'a', 'x', 'p'): slot = &face->maxp; break;
            case TAG('c', 'm', 'a', 'p'): slot = &face->cmap; break;
            case TAG('l', 'o', 'c', 'a'): slot = &face->loca; break;
            case TAG('g', 'l', 'y', 'f'): slot = &face->glyf; break;
            case TAG('O', 'S', '/', '2'): slot = &face->os2; break;
            case TAG('n', 'a', 'm', 'e'): slot = &face->name; break;
            case TAG('G', 'P', 'O', 'S'): slot = &face->gpos; break;
            case TAG('k', 'e', 'r', 'n'): slot = &face->kern; break;
            case TAG('C', 'F', 'F', ' '): case TAG('C', 'F', 'F', '2'): cff = 1; break;
            case TAG('f', 'v', 'a', 'r'): variable = 1; break;
            default: break;
        }
        if (slot == NULL) continue;
        if (slot->present) return malformed(error, "a table is listed twice");
        slot->present = 1;
        slot->offset = u32(record + 8);
        slot->length = u32(record + 12);
    }
    if (cff || !face->glyf.present || !face->loca.present) {
        font_fail(error, "unsupported-outline-format",
            cff ? "The font has CFF outlines; only TrueType outlines are supported."
                : "The font has no TrueType outlines (glyf and loca).");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (variable) {
        font_fail(error, "variable-font-unsupported", "The font is variable; pass one static instance of it.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (!face->head.present || !face->hhea.present || !face->hmtx.present || !face->maxp.present ||
        !face->cmap.present) {
        return malformed(error, "a required table (head, hhea, hmtx, maxp, cmap) is missing");
    }
    {
        const uint8_t *head = data + face->head.offset;
        if (face->head.length < 54u || u32(head + 12) != 0x5F0F3CF5u) return malformed(error, "the head table");
        face->units_per_em = u16(head + 18);
        if (face->units_per_em < 16u || face->units_per_em > 16384u) return malformed(error, "unitsPerEm");
        if (s16(head + 50) != 0 && s16(head + 50) != 1) return malformed(error, "indexToLocFormat");
        face->long_offsets = s16(head + 50) == 1;
    }
    if (face->maxp.length < 6u) return malformed(error, "the maxp table");
    face->glyph_count = u16(data + face->maxp.offset + 4);
    if (face->glyph_count == 0) return malformed(error, "the font has no glyphs");
    if (face->hhea.length < 36u) return malformed(error, "the hhea table");
    face->metric_count = u16(data + face->hhea.offset + 34);
    if (face->metric_count == 0 || face->metric_count > face->glyph_count ||
        (uint64_t)face->hmtx.length < 4u * (uint64_t)face->metric_count + 2u * (uint64_t)(face->glyph_count - face->metric_count)) {
        return malformed(error, "the horizontal metrics");
    }
    if ((uint64_t)face->loca.length < (face->long_offsets ? 4u : 2u) * ((uint64_t)face->glyph_count + 1u)) {
        return malformed(error, "the loca table is shorter than its glyphs");
    }
    if (face->os2.present && face->os2.length >= 90u) {
        const uint8_t *os2 = data + face->os2.offset;
        if (u16(os2) >= 2u && s16(os2 + 88) > 0) face->cap_height = s16(os2 + 88);
    }
    return choose_cmap(face, error);
}

/* --- Names ------------------------------------------------------------------------------------ */

static void utf8_append(char *out, size_t capacity, size_t *size, uint32_t code) {
    char bytes[4];
    size_t length;
    if (code < 0x80u) {
        bytes[0] = (char)code;
        length = 1;
    } else if (code < 0x800u) {
        bytes[0] = (char)(0xC0u | (code >> 6));
        bytes[1] = (char)(0x80u | (code & 0x3Fu));
        length = 2;
    } else if (code < 0x10000u) {
        bytes[0] = (char)(0xE0u | (code >> 12));
        bytes[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        bytes[2] = (char)(0x80u | (code & 0x3Fu));
        length = 3;
    } else {
        bytes[0] = (char)(0xF0u | (code >> 18));
        bytes[1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
        bytes[2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        bytes[3] = (char)(0x80u | (code & 0x3Fu));
        length = 4;
    }
    /* A name that does not fit is cut at a character, never inside one. */
    if (*size + length >= capacity) return;
    memcpy(out + *size, bytes, length);
    *size += length;
    out[*size] = '\0';
}

/** Windows English first, then any Windows language, then Unicode, then Macintosh Roman. */
static unsigned name_rank(uint16_t platform, uint16_t encoding, uint16_t language) {
    if (platform == 3u && (encoding == 1u || encoding == 10u)) return language == 0x0409u ? 4u : 3u;
    if (platform == 0u) return 2u;
    if (platform == 1u && encoding == 0u && language == 0u) return 1u;
    return 0;
}

static int read_name(const font_face *face, uint16_t id, char *out, size_t capacity) {
    const uint8_t *table = face->data + face->name.offset;
    const uint32_t length = face->name.length;
    uint32_t count, storage;
    unsigned best = 0;
    const uint8_t *chosen = NULL;
    uint32_t chosen_length = 0;
    unsigned chosen_platform = 0;
    out[0] = '\0';
    if (!face->name.present || length < 6u) return 0;
    count = u16(table + 2);
    storage = u16(table + 4);
    if (6u + 12u * (uint64_t)count > length) return 0;
    for (uint32_t at = 0; at < count; ++at) {
        const uint8_t *record = table + 6u + 12u * at;
        const unsigned rank = name_rank(u16(record), u16(record + 2), u16(record + 4));
        const uint32_t start = storage + u16(record + 10), bytes = u16(record + 8);
        if (u16(record + 6) != id || rank <= best || !inside(length, start, bytes)) continue;
        best = rank;
        chosen = table + start;
        chosen_length = bytes;
        chosen_platform = u16(record);
    }
    if (chosen == NULL) return 0;
    {
        size_t size = 0;
        if (chosen_platform == 1u) {
            /* Macintosh Roman: the ASCII half is shared; the rest is not worth a table here. */
            for (uint32_t at = 0; at < chosen_length; ++at) {
                utf8_append(out, capacity, &size, chosen[at] < 0x80u ? chosen[at] : (uint32_t)'?');
            }
        } else {
            for (uint32_t at = 0; at + 1u < chosen_length; at += 2u) {
                uint32_t code = u16(chosen + at);
                if (code >= 0xD800u && code <= 0xDBFFu && at + 3u < chosen_length &&
                    u16(chosen + at + 2u) >= 0xDC00u && u16(chosen + at + 2u) <= 0xDFFFu) {
                    code = 0x10000u + ((code - 0xD800u) << 10) + (u16(chosen + at + 2u) - 0xDC00u);
                    at += 2u;
                } else if (code >= 0xD800u && code <= 0xDFFFu) {
                    code = 0xFFFDu;
                }
                utf8_append(out, capacity, &size, code);
            }
        }
    }
    return out[0] != '\0';
}

void font_face_names(const font_face *face, font_source_info *info) {
    if (!read_name(face, 16, info->family, sizeof info->family)) {
        (void)read_name(face, 1, info->family, sizeof info->family);
    }
    if (!read_name(face, 17, info->style, sizeof info->style)) {
        (void)read_name(face, 2, info->style, sizeof info->style);
    }
}

/* --- Outlines --------------------------------------------------------------------------------- */

static int reserve_points(font_contours *contours, size_t needed) {
    if (needed <= contours->capacity) return 1;
    {
        size_t capacity = contours->capacity == 0 ? 64u : contours->capacity;
        font_point *grown;
        while (capacity < needed) capacity *= 2u;
        grown = realloc(contours->points, capacity * sizeof *grown);
        if (grown == NULL) return 0;
        contours->points = grown;
        contours->capacity = capacity;
    }
    return 1;
}

static int add_end(font_contours *contours, size_t end) {
    if (contours->contour_count == contours->contour_capacity) {
        const size_t capacity = contours->contour_capacity == 0 ? 8u : contours->contour_capacity * 2u;
        size_t *grown = realloc(contours->ends, capacity * sizeof *grown);
        if (grown == NULL) return 0;
        contours->ends = grown;
        contours->contour_capacity = capacity;
    }
    contours->ends[contours->contour_count++] = end;
    return 1;
}

void font_contours_free(font_contours *contours) {
    if (contours == NULL) return;
    free(contours->points);
    free(contours->ends);
    memset(contours, 0, sizeof *contours);
}

static edds_status out_of_memory(edds_error *error) {
    font_fail(error, "allocation-failed", "Memory for a glyph outline could not be allocated.");
    return EDDS_INTERNAL_FAILURE;
}

static edds_status glyph_limit(edds_error *error) {
    font_fail(error, "glyph-size-limit", "A glyph has more than %u points or %u contours.",
        FONT_MAX_GLYPH_POINTS, FONT_MAX_GLYPH_CONTOURS);
    return EDDS_INVALID_INPUT;
}

static edds_status read_simple(
    const uint8_t *glyph,
    uint32_t size,
    uint32_t contour_total,
    font_contours *out,
    edds_error *error
) {
    const size_t base = out->count;
    uint32_t at = 10;
    uint32_t point_total = 0;
    uint8_t *flags;
    int32_t x = 0, y = 0;
    if (contour_total == 0) return EDDS_OK;
    if (out->contour_count + contour_total > FONT_MAX_GLYPH_CONTOURS) return glyph_limit(error);
    if (!inside(size, at, 2u * (uint64_t)contour_total + 2u)) return malformed(error, "a glyph's contour list");
    for (uint32_t contour = 0; contour < contour_total; ++contour) {
        const uint32_t end = u16(glyph + at + 2u * contour) + 1u;
        if (end <= point_total && !(contour == 0 && end == 0)) return malformed(error, "contour ends out of order");
        point_total = end;
    }
    at += 2u * contour_total;
    if (out->count + point_total > FONT_MAX_GLYPH_POINTS) return glyph_limit(error);
    at += 2u + u16(glyph + at);
    if (at > size) return malformed(error, "glyph instructions run past the glyph");
    flags = malloc(point_total == 0 ? 1u : point_total);
    if (flags == NULL || !reserve_points(out, out->count + point_total)) {
        free(flags);
        return out_of_memory(error);
    }
    for (uint32_t point = 0; point < point_total;) {
        uint8_t flag;
        uint32_t repeat = 0;
        if (at >= size) goto truncated;
        flag = glyph[at++];
        if ((flag & 0x08u) != 0) {
            if (at >= size) goto truncated;
            repeat = glyph[at++];
        }
        if (point + repeat + 1u > point_total) goto truncated;
        for (uint32_t copy = 0; copy <= repeat; ++copy) flags[point++] = flag;
    }
    for (uint32_t point = 0; point < point_total; ++point) {
        const uint8_t flag = flags[point];
        if ((flag & 0x02u) != 0) {
            if (at >= size) goto truncated;
            x += (flag & 0x10u) != 0 ? glyph[at] : -(int32_t)glyph[at];
            ++at;
        } else if ((flag & 0x10u) == 0) {
            if (at + 2u > size) goto truncated;
            x += s16(glyph + at);
            at += 2u;
        }
        out->points[base + point].x = x;
        out->points[base + point].on_curve = (flag & 0x01u) != 0;
    }
    for (uint32_t point = 0; point < point_total; ++point) {
        const uint8_t flag = flags[point];
        if ((flag & 0x04u) != 0) {
            if (at >= size) goto truncated;
            y += (flag & 0x20u) != 0 ? glyph[at] : -(int32_t)glyph[at];
            ++at;
        } else if ((flag & 0x20u) == 0) {
            if (at + 2u > size) goto truncated;
            y += s16(glyph + at);
            at += 2u;
        }
        out->points[base + point].y = y;
    }
    free(flags);
    out->count = base + point_total;
    for (uint32_t contour = 0; contour < contour_total; ++contour) {
        if (!add_end(out, base + u16(glyph + 10u + 2u * contour) + 1u)) return out_of_memory(error);
    }
    return EDDS_OK;

truncated:
    free(flags);
    return malformed(error, "a glyph's points run past its data");
}

static double f2dot14(const uint8_t *at) {
    return (double)s16(at) / 16384.0;
}

typedef struct read_state {
    uint32_t components;
} read_state;

static edds_status read_glyph(
    const font_face *face,
    uint32_t glyph,
    uint32_t depth,
    read_state *state,
    font_contours *out,
    uint32_t *advance,
    edds_error *error
);

static edds_status read_composite(
    const font_face *face,
    const uint8_t *data,
    uint32_t size,
    uint32_t depth,
    read_state *state,
    font_contours *out,
    uint32_t *advance,
    edds_error *error
) {
    const size_t base = out->count;
    uint32_t at = 10;
    uint16_t flags;
    do {
        uint32_t component;
        double a = 1, b = 0, c = 0, d = 1, dx, dy;
        int32_t first, second;
        font_contours child = { 0 };
        uint32_t child_advance = 0;
        edds_status status;
        if (++state->components > FONT_MAX_COMPONENTS) {
            font_fail(error, "glyph-size-limit", "A glyph has more than %u components.", FONT_MAX_COMPONENTS);
            return EDDS_INVALID_INPUT;
        }
        if (!inside(size, at, 4u)) return malformed(error, "a component record");
        flags = u16(data + at);
        component = u16(data + at + 2u);
        at += 4u;
        if (component >= face->glyph_count) return malformed(error, "a component names a glyph the font lacks");
        if ((flags & ARG_1_AND_2_ARE_WORDS) != 0) {
            if (!inside(size, at, 4u)) return malformed(error, "a component record");
            first = (flags & ARGS_ARE_XY_VALUES) != 0 ? s16(data + at) : u16(data + at);
            second = (flags & ARGS_ARE_XY_VALUES) != 0 ? s16(data + at + 2u) : u16(data + at + 2u);
            at += 4u;
        } else {
            if (!inside(size, at, 2u)) return malformed(error, "a component record");
            first = (flags & ARGS_ARE_XY_VALUES) != 0 ? (int8_t)data[at] : data[at];
            second = (flags & ARGS_ARE_XY_VALUES) != 0 ? (int8_t)data[at + 1u] : data[at + 1u];
            at += 2u;
        }
        if ((flags & WE_HAVE_A_SCALE) != 0) {
            if (!inside(size, at, 2u)) return malformed(error, "a component transform");
            a = d = f2dot14(data + at);
            at += 2u;
        } else if ((flags & WE_HAVE_AN_X_AND_Y_SCALE) != 0) {
            if (!inside(size, at, 4u)) return malformed(error, "a component transform");
            a = f2dot14(data + at);
            d = f2dot14(data + at + 2u);
            at += 4u;
        } else if ((flags & WE_HAVE_A_TWO_BY_TWO) != 0) {
            if (!inside(size, at, 8u)) return malformed(error, "a component transform");
            a = f2dot14(data + at);
            b = f2dot14(data + at + 2u);
            c = f2dot14(data + at + 4u);
            d = f2dot14(data + at + 6u);
            at += 8u;
        }
        status = read_glyph(face, component, depth + 1u, state, &child, &child_advance, error);
        if (status != EDDS_OK) {
            font_contours_free(&child);
            return status;
        }
        if ((flags & ARGS_ARE_XY_VALUES) != 0) {
            dx = first;
            dy = second;
            if ((flags & SCALED_COMPONENT_OFFSET) != 0 && (flags & UNSCALED_COMPONENT_OFFSET) == 0) {
                const double x = dx, y = dy;
                dx = a * x + c * y;
                dy = b * x + d * y;
            }
        } else {
            /* Point matching: this component's point `second` lands on the composite's point `first`. */
            if ((size_t)first >= out->count - base || (size_t)second >= child.count) {
                font_contours_free(&child);
                return malformed(error, "a component matches a point that does not exist");
            }
            {
                const font_point parent = out->points[base + (size_t)first];
                const font_point own = child.points[second];
                dx = parent.x - (a * own.x + c * own.y);
                dy = parent.y - (b * own.x + d * own.y);
            }
        }
        if (out->count + child.count > FONT_MAX_GLYPH_POINTS ||
            out->contour_count + child.contour_count > FONT_MAX_GLYPH_CONTOURS) {
            font_contours_free(&child);
            return glyph_limit(error);
        }
        if (!reserve_points(out, out->count + child.count)) {
            font_contours_free(&child);
            return out_of_memory(error);
        }
        {
            const size_t offset = out->count;
            for (size_t point = 0; point < child.count; ++point) {
                const font_point source = child.points[point];
                font_point *moved = &out->points[offset + point];
                moved->x = a * source.x + c * source.y + dx;
                moved->y = b * source.x + d * source.y + dy;
                moved->on_curve = source.on_curve;
            }
            out->count += child.count;
            for (size_t contour = 0; contour < child.contour_count; ++contour) {
                if (!add_end(out, offset + child.ends[contour])) {
                    font_contours_free(&child);
                    return out_of_memory(error);
                }
            }
        }
        if ((flags & USE_MY_METRICS) != 0) *advance = child_advance;
        font_contours_free(&child);
    } while ((flags & MORE_COMPONENTS) != 0);
    return EDDS_OK;
}

static edds_status read_glyph(
    const font_face *face,
    uint32_t glyph,
    uint32_t depth,
    read_state *state,
    font_contours *out,
    uint32_t *advance,
    edds_error *error
) {
    const uint8_t *loca = face->data + face->loca.offset;
    uint32_t start, end;
    int16_t contours;
    *advance = font_face_advance(face, glyph);
    if (depth > FONT_MAX_COMPONENT_DEPTH) {
        font_fail(error, "glyph-size-limit", "Composite glyphs nest deeper than %u levels.", FONT_MAX_COMPONENT_DEPTH);
        return EDDS_INVALID_INPUT;
    }
    if (face->long_offsets) {
        start = u32(loca + 4u * glyph);
        end = u32(loca + 4u * glyph + 4u);
    } else {
        start = 2u * u16(loca + 2u * glyph);
        end = 2u * u16(loca + 2u * glyph + 2u);
    }
    if (start > end || end > face->glyf.length) return malformed(error, "a glyph lies outside the glyf table");
    if (start == end) return EDDS_OK;
    if (end - start < 10u) return malformed(error, "a glyph header is truncated");
    contours = s16(face->data + face->glyf.offset + start);
    if (contours >= 0) {
        return read_simple(face->data + face->glyf.offset + start, end - start, (uint32_t)contours, out, error);
    }
    return read_composite(face, face->data + face->glyf.offset + start, end - start, depth, state, out, advance, error);
}

edds_status font_face_contours(
    const font_face *face,
    uint32_t glyph,
    font_contours *contours,
    uint32_t *advance,
    edds_error *error
) {
    read_state state = { 0 };
    edds_status status;
    memset(contours, 0, sizeof *contours);
    if (glyph >= face->glyph_count) return malformed(error, "a glyph index is out of range");
    status = read_glyph(face, glyph, 0, &state, contours, advance, error);
    if (status != EDDS_OK) font_contours_free(contours);
    return status;
}
