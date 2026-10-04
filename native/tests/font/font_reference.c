/*
 * The independent check of a generated font. It shares no code with the generator: its own FNT5
 * reader, its own EDDS and LZ4 reader, its own rasterizer of the outlines the fixture planted, and
 * the engine's shader formula to rebuild every glyph from the atlas at four times its resolution.
 * An optional golden file holds the pairs fontTools read from the same font in a dev environment.
 *
 * usage: enfusion-font-reference gpos|kern SIZE FONT.fnt ATLAS.edds [GOLDEN_PAIRS.json]
 */
#include "font_fixture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_RANGE    8.4852813742385702
#define SUPERSAMPLE    4
/* A mismatch closer than this many 4× pixels to the true outline is antialiasing, not a fault. */
#define BOUNDARY_NOISE 2

typedef struct blob {
    uint8_t *data;
    size_t   size;
} blob;

static int failures = 0;

static void fail(const char *format, const char *detail, long value) {
    fprintf(stderr, format, detail, value);
    fputc('\n', stderr);
    ++failures;
}

static int load(const char *path, blob *out) {
    FILE *file;
    long  size;
    out->data = NULL;
    out->size = 0;
#ifdef _WIN32
    if (fopen_s(&file, path, "rb") != 0) {
        file = NULL;
    }
#else
    file = fopen(path, "rb");
#endif
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    out->data = malloc((size_t)size + 1u);
    out->size = (size_t)size;
    if (out->data == NULL || fread(out->data, 1, out->size, file) != out->size) {
        fclose(file);
        free(out->data);
        return 0;
    }
    fclose(file);
    return 1;
}

static uint32_t be32(const uint8_t *at) {
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

static uint32_t le32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static uint16_t le16(const uint8_t *at) {
    return (uint16_t)(at[0] | (at[1] << 8));
}

static float le_float(const uint8_t *at) {
    const uint32_t raw = le32(at);
    float          value;
    memcpy(&value, &raw, sizeof value);
    return value;
}

/* --- FNT5, read the way the engine reads it ---------------------------------------------------- */

typedef struct glyph_box {
    uint32_t code;
    uint16_t x, y, w, h;
    int16_t  bx, by, advance;
} glyph_box;

typedef struct kern_pair {
    uint16_t left, right;
    int32_t  value;
} kern_pair;

typedef struct fnt {
    char       name[256];
    int32_t    size, zero, cell;
    uint8_t    type, bold, italic;
    float      a, b, c;
    int16_t    r;
    glyph_box *glyphs;
    uint32_t   glyph_count;
    kern_pair *pairs;
    uint32_t   pair_count;
} fnt;

static int read_fnt(const blob *file, fnt *out) {
    size_t         at   = 12;
    const uint8_t *glps = NULL, *tcrd = NULL;
    uint32_t       glps_size = 0, tcrd_size = 0;
    int            head = 0;
    memset(out, 0, sizeof *out);
    if (file->size < 12 || memcmp(file->data, "FORM", 4) != 0 || memcmp(file->data + 8, "FNT5", 4) != 0 ||
        be32(file->data + 4) + 8u != file->size) {
        return 0;
    }
    while (at + 8u <= file->size) {
        const uint8_t *chunk = file->data + at + 8u;
        const uint32_t size  = be32(file->data + at + 4u);
        if (size > file->size - at - 8u) {
            return 0;
        }
        if (memcmp(file->data + at, "HEAD", 4) == 0) {
            const uint32_t name  = le32(chunk);
            const uint8_t *field = chunk + 4u + name;
            if (name == 0 || name > 255u || size != 4u + name + 29u || chunk[4u + name - 1u] != 0) {
                return 0;
            }
            memcpy(out->name, chunk + 4, name);
            out->size   = (int32_t)le32(field);
            out->zero   = (int32_t)le32(field + 4);
            out->type   = field[8];
            out->cell   = (int32_t)le32(field + 9);
            out->a      = le_float(field + 13);
            out->b      = le_float(field + 17);
            out->r      = (int16_t)le16(field + 21);
            out->bold   = field[23];
            out->italic = field[24];
            out->c      = le_float(field + 25);
            head        = 1;
        } else if (memcmp(file->data + at, "GLPS", 4) == 0) {
            glps      = chunk;
            glps_size = size;
        } else if (memcmp(file->data + at, "TCRD", 4) == 0) {
            tcrd      = chunk;
            tcrd_size = size;
        } else if (memcmp(file->data + at, "KERN", 4) == 0) {
            if (size % 8u != 0) {
                return 0;
            }
            out->pair_count = size / 8u;
            out->pairs      = malloc((out->pair_count + 1u) * sizeof *out->pairs);
            if (out->pairs == NULL) {
                return 0;
            }
            for (uint32_t pair = 0; pair < out->pair_count; ++pair) {
                out->pairs[pair].left  = le16(chunk + 8u * pair);
                out->pairs[pair].right = le16(chunk + 8u * pair + 2u);
                out->pairs[pair].value = (int32_t)le32(chunk + 8u * pair + 4u);
            }
        } else {
            return 0;
        }
        at += 8u + size;
    }
    if (at != file->size || !head || glps == NULL || tcrd == NULL || glps_size < 16u) {
        return 0;
    }
    {
        const uint32_t count = le32(glps + 8), ranges = le32(glps + 12);
        uint32_t       index = 0;
        if (glps_size != 16u + 8u * ranges || tcrd_size != 14u * count) {
            return 0;
        }
        out->glyph_count = count;
        out->glyphs      = malloc((count + 1u) * sizeof *out->glyphs);
        if (out->glyphs == NULL) {
            return 0;
        }
        for (uint32_t range = 0; range < ranges; ++range) {
            const uint32_t first  = le32(glps + 16u + 8u * range);
            const uint32_t length = le16(glps + 20u + 8u * range);
            if (le16(glps + 22u + 8u * range) != 0) {
                return 0;
            }
            for (uint32_t step = 0; step < length; ++step) {
                const uint8_t *box = tcrd + 14u * index;
                if (index >= count) {
                    return 0;
                }
                out->glyphs[index].code    = first + step;
                out->glyphs[index].x       = le16(box);
                out->glyphs[index].y       = le16(box + 2);
                out->glyphs[index].w       = le16(box + 4);
                out->glyphs[index].h       = le16(box + 6);
                out->glyphs[index].bx      = (int16_t)le16(box + 8);
                out->glyphs[index].by      = (int16_t)le16(box + 10);
                out->glyphs[index].advance = (int16_t)le16(box + 12);
                ++index;
            }
        }
        if (index != count) {
            return 0;
        }
    }
    return 1;
}

/* --- EDDS: one BGRA8 surface behind an ENF1 table of COPY or LZ4 ------------------------------ */

static int lz4_block(const uint8_t *input, size_t size, uint8_t *output, size_t capacity, size_t *written) {
    size_t in = 0, out = *written;
    while (in < size) {
        const uint8_t token    = input[in++];
        size_t        literals = token >> 4, match;
        if (literals == 15u) {
            uint8_t more;
            do {
                if (in >= size) {
                    return 0;
                }
                more      = input[in++];
                literals += more;
            } while (more == 255u);
        }
        if (literals > size - in || literals > capacity - out) {
            return 0;
        }
        memcpy(output + out, input + in, literals);
        in  += literals;
        out += literals;
        if (in == size) {
            break;
        }
        if (size - in < 2u) {
            return 0;
        }
        {
            const size_t offset  = (size_t)input[in] | ((size_t)input[in + 1u] << 8);
            in                  += 2u;
            match                = (token & 15u) + 4u;
            if ((token & 15u) == 15u) {
                uint8_t more;
                do {
                    if (in >= size) {
                        return 0;
                    }
                    more   = input[in++];
                    match += more;
                } while (more == 255u);
            }
            if (offset == 0 || offset > out || match > capacity - out) {
                return 0;
            }
            for (size_t copy = 0; copy < match; ++copy, ++out) {
                output[out] = output[out - offset];
            }
        }
    }
    *written = out;
    return 1;
}

static int read_atlas(const blob *file, uint32_t *width, uint32_t *height, uint8_t **bgra) {
    const uint8_t *data = file->data;
    uint32_t       stored, decoded;
    const uint8_t *payload;
    if (file->size < 136u || memcmp(data, "DDS ", 4) != 0 || memcmp(data + 36, "ENF1", 4) != 0) {
        return 0;
    }
    *height = le32(data + 12);
    *width  = le32(data + 16);
    /* One level, 32-bit with alpha, masks of BGRA. */
    if (le32(data + 28) != 1u || le32(data + 80) != 0x41u || le32(data + 88) != 32u ||
        le32(data + 92) != 0x00FF0000u || le32(data + 104) != 0xFF000000u) {
        return 0;
    }
    stored  = le32(data + 132);
    payload = data + 136;
    if (stored != file->size - 136u) {
        return 0;
    }
    decoded = *width * *height * 4u;
    *bgra   = malloc(decoded);
    if (*bgra == NULL) {
        return 0;
    }
    if (memcmp(data + 128, "COPY", 4) == 0) {
        if (stored != decoded) {
            return 0;
        }
        memcpy(*bgra, payload, decoded);
        return 1;
    }
    if (memcmp(data + 128, "LZ4 ", 4) != 0 || stored < 8u || le32(payload) != decoded) {
        return 0;
    }
    {
        size_t at = 4, written = 0;
        for (;;) {
            uint32_t descriptor, block;
            if (stored - at < 4u) {
                return 0;
            }
            descriptor  = le32(payload + at);
            block       = descriptor & 0x7FFFFFFFu;
            at         += 4u;
            if (block > stored - at || !lz4_block(payload + at, block, *bgra, decoded, &written)) {
                return 0;
            }
            at += block;
            if ((descriptor & 0x80000000u) != 0) {
                break;
            }
        }
        return at == stored && written == decoded;
    }
}

/* --- The truth: the planted outline, flattened and filled by the nonzero rule ------------------- */

typedef struct segment {
    double x0, y0, x1, y1;
} segment;

typedef struct polygon {
    segment *segments;
    size_t   count;
    size_t   capacity;
    double   box[4];
    int      empty;
} polygon;

static void add_segment(polygon *out, double x0, double y0, double x1, double y1) {
    if (out->count == out->capacity) {
        out->capacity = out->capacity == 0 ? 256u : out->capacity * 2u;
        out->segments = realloc(out->segments, out->capacity * sizeof *out->segments);
        if (out->segments == NULL) {
            exit(3);
        }
    }
    out->segments[out->count].x0 = x0;
    out->segments[out->count].y0 = y0;
    out->segments[out->count].x1 = x1;
    out->segments[out->count].y1 = y1;
    ++out->count;
}

static void include(polygon *out, double x, double y) {
    if (out->empty) {
        out->box[0] = out->box[2] = x;
        out->box[1] = out->box[3] = y;
        out->empty                = 0;
        return;
    }
    if (x < out->box[0]) {
        out->box[0] = x;
    }
    if (y < out->box[1]) {
        out->box[1] = y;
    }
    if (x > out->box[2]) {
        out->box[2] = x;
    }
    if (y > out->box[3]) {
        out->box[3] = y;
    }
}

/** A quadratic piece as 64 lines, and its exact extremes for the box. */
static void add_curve(polygon *out, double x0, double y0, double cx, double cy, double x1, double y1, int curved) {
    if (!curved) {
        add_segment(out, x0, y0, x1, y1);
        include(out, x0, y0);
        include(out, x1, y1);
        return;
    }
    {
        double previous_x = x0, previous_y = y0;
        for (int step = 1; step <= 64; ++step) {
            const double t = step / 64.0, s = 1.0 - t;
            const double x = s * s * x0 + 2 * s * t * cx + t * t * x1, y = s * s * y0 + 2 * s * t * cy + t * t * y1;
            add_segment(out, previous_x, previous_y, x, y);
            previous_x = x;
            previous_y = y;
        }
        include(out, x0, y0);
        include(out, x1, y1);
        for (int axis = 0; axis < 2; ++axis) {
            const double p0 = axis ? y0 : x0, p1 = axis ? cy : cx, p2 = axis ? y1 : x1;
            const double denominator = p0 - 2 * p1 + p2;
            if (denominator != 0) {
                const double t = (p0 - p1) / denominator;
                if (t > 0 && t < 1) {
                    const double s = 1.0 - t;
                    include(out, s * s * x0 + 2 * s * t * cx + t * t * x1, s * s * y0 + 2 * s * t * cy + t * t * y1);
                }
            }
        }
    }
}

/** TrueType contours in px: consecutive off-curve points imply the on-curve point between them. */
static void flatten(const font_fixture_outline *outline, double scale, polygon *out) {
    size_t start = 0;
    memset(out, 0, sizeof *out);
    out->empty = 1;
    for (size_t contour = 0; contour < outline->contour_count; ++contour) {
        const size_t end = outline->contour_ends[contour], count = end - start;
        double       points[128][2];
        int          on[128];
        size_t       total = 0;
        /* Spell the implied on-curve points out, so every curve is on-off-on. */
        for (size_t at = 0; at < count; ++at) {
            const font_fixture_point *here = &outline->points[start + at];
            const font_fixture_point *next = &outline->points[start + (at + 1u) % count];
            points[total][0]               = here->x * scale;
            points[total][1]               = here->y * scale;
            on[total++]                    = here->on_curve;
            if (!here->on_curve && !next->on_curve) {
                points[total][0] = (here->x + next->x) * 0.5 * scale;
                points[total][1] = (here->y + next->y) * 0.5 * scale;
                on[total++]      = 1;
            }
        }
        {
            size_t first = 0;
            while (!on[first]) {
                ++first;
            }
            for (size_t step = 0; step < total;) {
                const size_t a = (first + step) % total, b = (first + step + 1u) % total;
                if (on[b]) {
                    add_curve(out, points[a][0], points[a][1], 0, 0, points[b][0], points[b][1], 0);
                    step += 1u;
                } else {
                    const size_t c = (first + step + 2u) % total;
                    add_curve(out, points[a][0], points[a][1], points[b][0], points[b][1], points[c][0], points[c][1], 1);
                    step += 2u;
                }
            }
        }
        start = end;
    }
}

static int filled(const polygon *shape, double x, double y) {
    int winding = 0;
    for (size_t at = 0; at < shape->count; ++at) {
        const segment *edge = &shape->segments[at];
        if ((edge->y0 <= y && y < edge->y1) || (edge->y1 <= y && y < edge->y0)) {
            const double cross_x = edge->x0 + (y - edge->y0) * (edge->x1 - edge->x0) / (edge->y1 - edge->y0);
            if (cross_x > x) {
                winding += edge->y1 > edge->y0 ? 1 : -1;
            }
        }
    }
    return winding != 0;
}

/* --- Checks ------------------------------------------------------------------------------------ */

static double round_half_away(double value) {
    return value >= 0 ? floor(value + 0.5) : -floor(-value + 0.5);
}

static const glyph_box *box_of(const fnt *font, uint32_t code) {
    for (uint32_t at = 0; at < font->glyph_count; ++at) {
        if (font->glyphs[at].code == code) {
            return &font->glyphs[at];
        }
    }
    return NULL;
}

static double median3(double a, double b, double c) {
    return fmax(fmin(a, b), fmin(fmax(a, b), c));
}

/** The shader: bilinear between texel centres, median of three, ink from half up. */
static int inked(const uint8_t *bgra, uint32_t width, uint32_t height, double u, double v) {
    const double x = u - 0.5, y = v - 0.5;
    const int    x0 = (int)floor(x), y0 = (int)floor(y);
    const double fx = x - x0, fy = y - y0;
    double       channel[3];
    for (int c = 0; c < 3; ++c) {
        double corners[4];
        for (int corner = 0; corner < 4; ++corner) {
            int cx = x0 + (corner & 1), cy = y0 + (corner >> 1);
            if (cx < 0) {
                cx = 0;
            }
            if (cy < 0) {
                cy = 0;
            }
            if (cx >= (int)width) {
                cx = (int)width - 1;
            }
            if (cy >= (int)height) {
                cy = (int)height - 1;
            }
            /* Stored BGRA: the field's red is byte 2, green 1, blue 0. */
            corners[corner] = bgra[((size_t)cy * width + (size_t)cx) * 4u + (size_t)(2 - c)] / 255.0;
        }
        channel[c] = (corners[0] * (1 - fx) + corners[1] * fx) * (1 - fy) + (corners[2] * (1 - fx) + corners[3] * fx) * fy;
    }
    return median3(channel[0], channel[1], channel[2]) >= 0.5;
}

/**
 * Rebuilds one glyph at 4× over the quad the engine draws, the box plus five pixels, and counts
 * the samples that disagree with the outline farther than antialiasing reaches from its edge.
 */
static long compare_glyph(const polygon *truth, const glyph_box *box, int32_t cell,
    const uint8_t *bgra, uint32_t width, uint32_t height, long *ink) {
    const double   box_left = box->x + (cell - box->w) * 0.5, box_top = box->y + (cell - box->h) * 0.5;
    const int      across = (box->w + 10) * SUPERSAMPLE, down = (box->h + 10) * SUPERSAMPLE;
    unsigned char *reference = malloc((size_t)across * down), *rebuilt = malloc((size_t)across * down);
    long           faults = 0;
    *ink                  = 0;
    if (reference == NULL || rebuilt == NULL) {
        exit(3);
    }
    for (int row = 0; row < down; ++row) {
        for (int column = 0; column < across; ++column) {
            const double gx = box->bx - 5.0 + (column + 0.5) / SUPERSAMPLE;
            const double gy = box->by + 5.0 - (row + 0.5) / SUPERSAMPLE;
            const double u = box_left + (gx - box->bx), v = box_top + (box->by - gy);
            reference[row * across + column]  = (unsigned char)filled(truth, gx, gy);
            rebuilt[row * across + column]    = (unsigned char)inked(bgra, width, height, u, v);
            *ink                             += reference[row * across + column];
        }
    }
    for (int row = 0; row < down; ++row) {
        for (int column = 0; column < across; ++column) {
            int noise = 0;
            if (reference[row * across + column] == rebuilt[row * across + column]) {
                continue;
            }
            for (int dy = -BOUNDARY_NOISE; dy <= BOUNDARY_NOISE && !noise; ++dy) {
                for (int dx = -BOUNDARY_NOISE; dx <= BOUNDARY_NOISE && !noise; ++dx) {
                    const int r = row + dy, c = column + dx;
                    if (r < 0 || c < 0 || r >= down || c >= across) {
                        continue;
                    }
                    noise = reference[r * across + c] != reference[row * across + column];
                }
            }
            if (!noise) {
                ++faults;
            }
        }
    }
    free(reference);
    free(rebuilt);
    return faults;
}

/** The smallest power-of-two atlas, width at least height, with 1 px of zeros between cells. */
static int smallest_atlas(uint32_t cells, int32_t cell, uint32_t *width, uint32_t *height) {
    for (int area = 0; area <= 24; ++area) {
        for (int high = area / 2; high >= 0; --high) {
            const int wide = area - high;
            if (wide > 12) {
                break;
            }
            if ((uint64_t)(((1u << wide) + 1u) / (uint32_t)(cell + 1)) * (((1u << high) + 1u) / (uint32_t)(cell + 1)) >= cells) {
                *width  = 1u << wide;
                *height = 1u << high;
                return 1;
            }
        }
    }
    return 0;
}

/** `{"pairs": [[left, right, value], ...]}`: every integer after "pairs", three at a time. */
static int matches_golden(const char *path, const fnt *font) {
    blob        golden;
    const char *at;
    uint32_t    index = 0;
    if (!load(path, &golden)) {
        return 0;
    }
    golden.data[golden.size] = 0;
    at                       = strstr((const char *)golden.data, "\"pairs\"");
    if (at == NULL) {
        free(golden.data);
        return 0;
    }
    for (;;) {
        long values[3];
        int  found = 0;
        for (; found < 3 && *at != 0; ++at) {
            if (*at == '-' || (*at >= '0' && *at <= '9')) {
                char *end;
                values[found++] = strtol(at, &end, 10);
                at              = end - 1;
            }
        }
        if (found == 0) {
            break;
        }
        if (found != 3 || index >= font->pair_count || font->pairs[index].left != values[0] ||
            font->pairs[index].right != values[1] || font->pairs[index].value != values[2]) {
            free(golden.data);
            return 0;
        }
        ++index;
    }
    free(golden.data);
    return index == font->pair_count;
}

int main(int argc, char **argv) {
    font_fixture_variant     variant;
    const uint32_t          *codes = NULL;
    size_t                   code_count;
    const font_fixture_pair *planted = NULL;
    size_t                   planted_count;
    blob                     font_file, atlas_file;
    fnt                      font;
    uint32_t                 width = 0, height = 0, unique_cells = 0, expected_width, expected_height;
    uint8_t                 *bgra = NULL;
    double                   size, scale;
    int32_t                  largest = 0;
    if ((argc != 5 && argc != 6) || (strcmp(argv[1], "gpos") != 0 && strcmp(argv[1], "kern") != 0)) {
        fputs("usage: enfusion-font-reference gpos|kern SIZE FONT.fnt ATLAS.edds [GOLDEN_PAIRS.json]\n", stderr);
        return 2;
    }
    variant       = strcmp(argv[1], "gpos") == 0 ? FONT_FIXTURE_GPOS : FONT_FIXTURE_KERN;
    size          = atof(argv[2]);
    scale         = size / FONT_FIXTURE_UNITS_PER_EM;
    code_count    = font_fixture_codes(variant, &codes);
    planted_count = font_fixture_pairs(variant, &planted);
    if (!load(argv[3], &font_file) || !read_fnt(&font_file, &font)) {
        fputs("the FNT file is not a well-formed FNT5\n", stderr);
        return 1;
    }
    if (!load(argv[4], &atlas_file) || !read_atlas(&atlas_file, &width, &height, &bgra)) {
        fputs("the atlas is not one lossless BGRA8 surface without mips\n", stderr);
        return 1;
    }

    /* The header. */
    if (font.size != (int32_t)size || font.zero != 0) {
        fail("%s %ld", "HEAD size", font.size);
    }
    if (font.type != 2 || font.r != 8) {
        fail("%s %ld", "HEAD is not MSDF with R 8", font.r);
    }
    if (font.bold != 0 || font.italic != 0) {
        fail("%s %ld", "HEAD bold/italic", font.bold);
    }
    if (font.b != (float)size || font.c != (float)size) {
        fail("%s %ld", "HEAD B and C are not the size", (long)font.b);
    }
    {
        const int cap      = font_fixture_cap_height(variant);
        double    expected = cap * scale;
        if (cap < 0) {
            /* Without OS/2 cap height, the top of the planted H, whether or not the set holds it. */
            font_fixture_outline outline;
            polygon              truth;
            if (!font_fixture_outline_of(variant, 0x48u, &outline)) {
                fail("%s %ld", "no truth for H", 0);
            } else {
                flatten(&outline, scale, &truth);
                expected = truth.box[3];
                free(truth.segments);
                font_fixture_outline_free(&outline);
            }
        }
        if (font.a != (float)round_half_away(expected)) {
            fail("%s %ld", "HEAD cap height", (long)font.a);
        }
    }

    /* Every planted code, the box drawn for U+25A1, nothing else; each box from its outline. */
    {
        uint32_t expected = 0;
        for (size_t at = 0; at < code_count; ++at) {
            const glyph_box     *box = box_of(&font, codes[at]);
            font_fixture_outline outline;
            polygon              truth;
            ++expected;
            if (box == NULL) {
                fail("%s U+%04lX", "missing glyph", (long)codes[at]);
                continue;
            }
            if (!font_fixture_outline_of(variant, codes[at], &outline)) {
                if (codes[at] != 0x20u && codes[at] != 0xA0u) {
                    fail("%s U+%04lX", "no truth for", (long)codes[at]);
                }
                continue;
            }
            flatten(&outline, scale, &truth);
            if (box->advance != (int16_t)round_half_away(outline.advance * scale)) {
                fail("%s U+%04lX", "advance of", (long)codes[at]);
            }
            if (truth.empty) {
                if (box->w != 0 || box->h != 0) {
                    fail("%s U+%04lX", "an empty glyph has a box:", (long)codes[at]);
                }
            } else {
                const int32_t bx = (int32_t)floor(truth.box[0]), by = (int32_t)ceil(truth.box[3]);
                const int32_t w = (int32_t)ceil(truth.box[2]) - bx, h = by - (int32_t)floor(truth.box[1]);
                if (box->bx != bx || box->by != by || box->w != w || box->h != h) {
                    fprintf(stderr, "U+%04X box %d %d %d %d, outline gives %d %d %d %d\n", codes[at],
                        box->bx, box->by, box->w, box->h, bx, by, w, h);
                    ++failures;
                }
            }
            free(truth.segments);
            font_fixture_outline_free(&outline);
        }
        if (box_of(&font, 0x25A1u) == NULL) {
            fail("%s %ld", "no missing-glyph box", 0);
        }
        ++expected;
        if (font.glyph_count != expected) {
            fail("%s %ld", "glyph count", (long)font.glyph_count);
        }
    }

    /* Cells: one per glyph, inside the atlas, none overlapping, the size the largest box asks for. */
    for (uint32_t at = 0; at < font.glyph_count; ++at) {
        const glyph_box *box    = &font.glyphs[at];
        int              shared = 0;
        if (box->w > largest) {
            largest = box->w;
        }
        if (box->h > largest) {
            largest = box->h;
        }
        for (uint32_t other = 0; other < at; ++other) {
            const glyph_box *before = &font.glyphs[other];
            const int        same   = before->x == box->x && before->y == box->y;
            const int        apart  = before->x + font.cell + 1 <= box->x || box->x + font.cell + 1 <= before->x ||
                before->y + font.cell + 1 <= box->y || box->y + font.cell + 1 <= before->y;
            if (same) {
                shared = 1;
            } else if (!apart) {
                fail("%s U+%04lX", "a cell overlaps or touches another:", (long)box->code);
            }
        }
        if (!shared) {
            ++unique_cells;
        }
        if (box->x + font.cell > (int32_t)width || box->y + font.cell > (int32_t)height) {
            fail("%s U+%04lX", "a cell runs past the atlas:", (long)box->code);
        }
    }
    if (font.cell != largest + 10) {
        fail("%s %ld", "cell", font.cell);
    }
    if (!smallest_atlas(unique_cells, font.cell, &expected_width, &expected_height) ||
        width != expected_width || height != expected_height) {
        fail("%s %ld", "the atlas is not the smallest power of two that holds its cells; width", (long)width);
    }

    /* Alpha 255 everywhere; zeros everywhere outside the cells. */
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *pixel   = bgra + ((size_t)y * width + x) * 4u;
            int            in_cell = 0;
            for (uint32_t at = 0; at < font.glyph_count && !in_cell; ++at) {
                in_cell = x >= font.glyphs[at].x && x < (uint32_t)(font.glyphs[at].x + font.cell) &&
                    y >= font.glyphs[at].y && y < (uint32_t)(font.glyphs[at].y + font.cell);
            }
            if (pixel[3] != 255u) {
                fail("%s %ld", "alpha is not 255 at row", (long)y);
                y = height;
                break;
            }
            if (!in_cell && (pixel[0] | pixel[1] | pixel[2]) != 0) {
                fail("%s %ld", "a pixel between cells is not zero, row", (long)y);
                y = height;
                break;
            }
        }
    }

    /* Every glyph rebuilt by the shader formula agrees with its outline beyond antialiasing. */
    {
        const font_fixture_frame frame = font_fixture_missing_box();
        for (uint32_t at = 0; at < font.glyph_count; ++at) {
            const glyph_box     *box = &font.glyphs[at];
            font_fixture_outline outline;
            polygon              truth;
            long                 faults, ink;
            if (box->code == 0x25A1u) {
                /* The drawn box: what the generator's frame is specified to be. */
                static size_t      frame_ends[2] = { 4, 8 };
                const double       e             = FONT_FIXTURE_UNITS_PER_EM;
                const double       x0 = frame.left * e, y0 = frame.bottom * e;
                const double       x1 = (frame.left + frame.side) * e, y1 = (frame.bottom + frame.side) * e;
                const double       t               = frame.stroke * e;
                font_fixture_point frame_points[8] = {
                    { x0, y0, 1 }, { x0, y1, 1 }, { x1, y1, 1 }, { x1, y0, 1 },
                    { x0 + t, y0 + t, 1 }, { x1 - t, y0 + t, 1 }, { x1 - t, y1 - t, 1 }, { x0 + t, y1 - t, 1 }
                };
                outline.points        = frame_points;
                outline.point_count   = 8;
                outline.contour_ends  = frame_ends;
                outline.contour_count = 2;
                outline.advance       = (int)round_half_away(frame.advance * e);
                flatten(&outline, scale, &truth);
                if (box->advance != (int16_t)round_half_away(frame.advance * size)) {
                    fail("%s %ld", "drawn box advance", box->advance);
                }
            } else if (!font_fixture_outline_of(variant, box->code, &outline)) {
                continue;
            } else {
                flatten(&outline, scale, &truth);
                font_fixture_outline_free(&outline);
            }
            if (!truth.empty) {
                faults = compare_glyph(&truth, box, font.cell, bgra, width, height, &ink);
                if (faults != 0 || ink == 0) {
                    fprintf(stderr, "U+%04X: %ld samples disagree with the outline beyond antialiasing\n", box->code, faults);
                    ++failures;
                }
            }
            free(truth.segments);
        }
    }

    /* The field runs 0..1 over 1.5 × R / √2 atlas pixels: the commonest median step says how far. */
    {
        long histogram[256] = { 0 };
        long best           = 0;
        int  mode           = 0;
        for (uint32_t at = 0; at < font.glyph_count; ++at) {
            const glyph_box *box = &font.glyphs[at];
            for (int y = box->y; y < box->y + font.cell; ++y) {
                for (int x = box->x; x + 1 < box->x + font.cell; ++x) {
                    const uint8_t *a = bgra + ((size_t)y * width + (size_t)x) * 4u, *b = a + 4;
                    const int      ma = (int)median3(a[0], a[1], a[2]), mb = (int)median3(b[0], b[1], b[2]);
                    if (ma > 8 && ma < 247 && mb > 8 && mb < 247 && ma != mb) {
                        ++histogram[abs(ma - mb)];
                    }
                }
            }
        }
        for (int step = 1; step < 256; ++step) {
            if (histogram[step] > best) {
                best = histogram[step];
                mode = step;
            }
        }
        if (mode == 0 || fabs(255.0 / mode - FIELD_RANGE) > 0.4) {
            fail("%s %ld", "the measured field range does not match 1.5 R / sqrt 2; median step", mode);
        } else {
            fprintf(stderr, "field range %.3f px (median step %d)\n", 255.0 / mode, mode);
        }
    }

    /* KERN: the planted pairs, in whole atlas pixels, BMP only, no zeros, by (left << 16) | right. */
    {
        kern_pair expected[64];
        uint32_t  expected_count = 0;
        for (size_t at = 0; at < planted_count; ++at) {
            const double value = round_half_away(planted[at].value * scale);
            if (value == 0 || planted[at].left > 0xFFFFu || planted[at].right > 0xFFFFu) {
                continue;
            }
            expected[expected_count].left  = (uint16_t)planted[at].left;
            expected[expected_count].right = (uint16_t)planted[at].right;
            expected[expected_count].value = (int32_t)value;
            ++expected_count;
        }
        if (font.pair_count != expected_count) {
            fail("%s %ld", "KERN pair count", (long)font.pair_count);
        } else {
            for (uint32_t at = 0; at < expected_count; ++at) {
                if (font.pairs[at].left != expected[at].left || font.pairs[at].right != expected[at].right ||
                    font.pairs[at].value != expected[at].value) {
                    fprintf(stderr, "KERN %u: %04X %04X %d, planted %04X %04X %d\n", at, font.pairs[at].left,
                        font.pairs[at].right, font.pairs[at].value, expected[at].left, expected[at].right, expected[at].value);
                    ++failures;
                }
            }
        }
    }

    if (argc == 6 && !matches_golden(argv[5], &font)) {
        fail("%s (%ld)", "KERN differs from the pairs fontTools read into the golden file", 0);
    }

    free(font.glyphs);
    free(font.pairs);
    free(bgra);
    free(font_file.data);
    free(atlas_file.data);
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
