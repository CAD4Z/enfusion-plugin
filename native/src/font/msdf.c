/*
 * The multi-channel signed distance field of one glyph. Edges are coloured so that at every sharp
 * corner the two sides feed different channels; the median of three channels then keeps the corner
 * sharp. Where the median would say inside and the outline says outside, or the other way round -
 * at a texel or anywhere the shader interpolates between texels - the texel falls back to the true
 * distance in all three channels.
 *
 * The edge colouring, the signed and pseudo-distances and the equation solvers are adapted from
 * msdfgen by Viktor Chlumsky, MIT licence; the notice is in THIRD-PARTY.md.
 */
#include "geometry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** An edge's colour: the channels of the field it feeds, one bit each. */
#define RED     1u
#define GREEN   2u
#define BLUE    4u
#define YELLOW  (RED | GREEN)
#define MAGENTA (RED | BLUE)
#define CYAN    (GREEN | BLUE)
#define WHITE   (RED | GREEN | BLUE)

/** Texels of field computed around the glyph box: the engine's 5 px quad plus one for filtering. */
#define MARGIN 6.0

/**
 * A sign the median disagrees with closer than this to the outline is antialiasing, not a fault.
 */
#define TOLERANCE 0.25

/** The most passes the correction makes over the blocks of a field. */
#define MOST_CORRECTION_PASSES 16

/**
 * A signed distance to an edge, and `dot`, which decides between two edges equally far away: the
 * smaller one wins.
 */
typedef struct signed_distance {
    double distance;
    double dot;
} signed_distance;

/** The vector scaled to length 1; a zero vector gives (0, 1). */
static font_vec normalized(font_vec a) {
    const double size = vec_length(a);

    return size == 0 ? vec_of(0, 1) : vec_times(a, 1.0 / size);
}

/** 1 for a positive value, -1 for any other, zero included. */
static double nonzero_sign(double value) {
    return value > 0 ? 1.0 : -1.0;
}

/* --- Colouring -------------------------------------------------------------------------------- */

/**
 * Moves `color` on to the colour of the next run of edges; `seed` makes the choice where there is
 * one, and is used up as it does.
 */
static void switch_color(unsigned *color, unsigned *seed, unsigned banned) {
    const unsigned combined = *color & banned;

    /* A colour sharing exactly one channel with `banned` becomes the other two channels. */
    if (combined == RED || combined == GREEN || combined == BLUE) {
        *color = combined ^ WHITE;
        return;
    }

    /* No colour yet, or white: cyan, magenta or yellow, as the seed picks. */
    if (*color == 0 || *color == WHITE) {
        static const unsigned start[3] = { CYAN, MAGENTA, YELLOW };

        *color  = start[*seed % 3u];
        *seed  /= 3u;
        return;
    }

    /* Any other colour: its channels rotated by one place or two, as the seed's lowest bit says. */
    {
        const unsigned shifted = *color << (1u + (*seed & 1u));

        *color   = (shifted | shifted >> 3) & WHITE;
        *seed  >>= 1;
    }
}

/**
 * Which of three colours a part takes when one corner splits a loop into three runs. The answer is
 * -1, 0 or 1, for the first, middle and last run.
 */
static int trichotomy(size_t position, size_t count) {
    return (int)(3.0 + 2.875 * (double)position / (double)(count - 1u) - 1.4375 + 0.5) - 3;
}

/**
 * Whether the outline turns a corner from direction a to direction b: by a right angle or more, or
 * with a cross product beyond `threshold`.
 */
static int is_corner(font_vec a, font_vec b, double threshold) {
    return vec_dot(a, b) <= 0 || fabs(vec_cross(a, b)) > threshold;
}

/**
 * The simple colouring: a smooth loop takes one colour; a loop with one corner is split in three
 * runs around it; between consecutive corners the colour changes, and the last run of a loop is
 * kept from repeating the first one's channels. The seed is fixed, so a font is reproducible. On
 * success the caller owns `colored` and frees it with font_shape_free.
 */
static edds_status color_shape(const font_shape *loops, font_shape *colored, edds_error *error) {
    const double threshold = sin(3.0);
    unsigned     color     = WHITE;
    unsigned     seed      = 0;
    size_t       start     = 0;
    size_t      *corners   = malloc((loops->count == 0 ? 1u : loops->count) * sizeof *corners);

    memset(colored, 0, sizeof *colored);

    if (corners == NULL) {
        goto failed;
    }

    /* Room for every edge split in three, and three more. */
    colored->edges = malloc((loops->count * 3u + 3u) * sizeof *colored->edges);

    if (colored->edges == NULL) {
        goto failed;
    }

    colored->capacity = loops->count * 3u + 3u;

    /* Loop by loop: first its corners, then the colours of its edges. */
    for (size_t loop = 0; loop < loops->loop_count; ++loop) {
        const size_t     end          = loops->loop_ends[loop];
        const size_t     count        = end - start;
        const font_edge *edges        = loops->edges + start;
        size_t           corner_count = 0;

        /* The corners, each recorded as the edge that starts at it. */
        for (size_t at = 0; at < count; ++at) {
            const font_vec before = normalized(
                edge_direction(&edges[(at + count - 1u) % count], 1.0));
            const font_vec after = normalized(edge_direction(&edges[at], 0.0));

            if (is_corner(before, after, threshold)) {
                corners[corner_count++] = at;
            }
        }

        if (corner_count == 0) {
            /* No corner: the whole loop takes one colour. */
            switch_color(&color, &seed, 0);

            for (size_t at = 0; at < count; ++at) {
                font_edge edge = edges[at];

                edge.color = color;

                if (!font_shape_push(colored, &edge)) {
                    goto failed;
                }
            }
        } else if (corner_count == 1) {
            /* One corner: three runs from it round the loop, the middle one white. */
            unsigned colors[3];

            switch_color(&color, &seed, 0);
            colors[0] = color;
            colors[1] = WHITE;
            switch_color(&color, &seed, 0);
            colors[2] = color;

            if (count >= 3u) {
                for (size_t at = 0; at < count; ++at) {
                    font_edge edge = edges[(corners[0] + at) % count];

                    edge.color = colors[1 + trichotomy(at, count)];

                    if (!font_shape_push(colored, &edge)) {
                        goto failed;
                    }
                }
            } else {
                /*
                 * Too few edges for three runs: every edge splits in thirds, starting at the
                 * corner.
                 */
                const size_t parts = 3u * count;

                for (size_t at = 0; at < parts; ++at) {
                    font_edge edge = edge_part(&edges[(corners[0] + at / 3u) % count],
                        (double)(at % 3u) / 3.0, (double)(at % 3u + 1u) / 3.0);

                    edge.color = colors[1 + trichotomy(at, parts)];

                    if (!font_shape_push(colored, &edge)) {
                        goto failed;
                    }
                }
            }
        } else {
            /* Several corners: a run of one colour from each corner to the next. */
            size_t   spline = 0;
            unsigned initial;

            switch_color(&color, &seed, 0);
            initial = color;

            for (size_t at = 0; at < count; ++at) {
                const size_t index = (corners[0] + at) % count;
                font_edge    edge  = edges[index];

                /* A new colour at each corner; the last run's is chosen against the first's. */
                if (spline + 1u < corner_count && corners[spline + 1u] == index) {
                    ++spline;
                    switch_color(&color, &seed, spline == corner_count - 1u ? initial : 0u);
                }

                edge.color = color;

                if (!font_shape_push(colored, &edge)) {
                    goto failed;
                }
            }
        }

        start = end;
    }

    free(corners);
    return EDDS_OK;

failed:
    free(corners);
    font_shape_free(colored);
    font_fail(error, "allocation-failed", "Memory for a glyph field could not be allocated.");
    return EDDS_INTERNAL_FAILURE;
}

/* --- Distances -------------------------------------------------------------------------------- */

/** Whether a is nearer than b; at the same distance, whether its `dot` is smaller. */
static int closer(signed_distance a, signed_distance b) {
    return fabs(a.distance) < fabs(b.distance) ||
        (fabs(a.distance) == fabs(b.distance) && a.dot < b.dot);
}

/**
 * Real roots of x^3 + a*x^2 + b*x + c: trigonometric for three, Cardano for one. The roots go to
 * `roots`, and the return value says how many there are.
 */
static int solve_normed_cubic(double roots[3], double a, double b, double c) {
    const double pi = 3.14159265358979323846;

    /* The terms the roots are worked out from. */
    const double a2 = a * a;
    double       q  = (a2 - 3.0 * b) / 9.0;
    const double r  = (a * (2.0 * a2 - 9.0 * b) + 27.0 * c) / 54.0;
    const double r2 = r * r;
    const double q3 = q * q * q;

    /* What every root is moved back by at the end. */
    const double shift = a / 3.0;

    /* Three real roots, the trigonometric way. */
    if (r2 < q3) {
        double t = r / sqrt(q3);

        if (t < -1) {
            t = -1;
        }

        if (t > 1) {
            t = 1;
        }

        t        = acos(t);
        q        = -2.0 * sqrt(q);
        roots[0] = q * cos(t / 3.0) - shift;
        roots[1] = q * cos((t + 2.0 * pi) / 3.0) - shift;
        roots[2] = q * cos((t - 2.0 * pi) / 3.0) - shift;
        return 3;
    }

    /* One real root, Cardano's way, and a second one when u and v are equal or nearly. */
    {
        const double u = (r < 0 ? 1.0 : -1.0) * pow(fabs(r) + sqrt(r2 - q3), 1.0 / 3.0);
        const double v = u == 0 ? 0 : q / u;

        roots[0] = (u + v) - shift;

        if (u == v || fabs(u - v) < 1e-12 * fabs(u + v)) {
            roots[1] = -0.5 * (u + v) - shift;
            return 2;
        }

        return 1;
    }
}

/**
 * Real roots of a*x^3 + b*x^2 + c*x + d: the cubic divided through by a, or the quadratic when
 * there is no cubic term. The roots go to `roots`, and the return value says how many there are.
 */
static int solve_cubic(double roots[3], double a, double b, double c, double d) {
    if (a != 0) {
        const double normed = b / a;

        /* Beyond this ratio the cubic term is noise and the quadratic is the better answer. */
        if (fabs(normed) < 1e6) {
            return solve_normed_cubic(roots, normed, c / a, d / a);
        }
    }

    return quadratic_roots(roots, b, c, d);
}

/**
 * Signed distance from `origin` to an edge; positive on the edge's right, where the fill is.
 * `param` gets the parameter of the nearest point, below 0 or above 1 when it lies beyond an end.
 */
static signed_distance edge_distance(const font_edge *edge, font_vec origin, double *param) {
    signed_distance result;

    /* A line. */
    if (!edge->quad) {
        const font_vec aq = vec_minus(origin, edge->p[0]);
        const font_vec ab = vec_minus(edge->p[2], edge->p[0]);
        font_vec       nearer;
        double         endpoint;

        /* Where along the line the origin falls, and how far the nearer end is. */
        *param   = vec_dot(aq, ab) / vec_dot(ab, ab);
        nearer   = vec_minus(*param > 0.5 ? edge->p[2] : edge->p[0], origin);
        endpoint = vec_length(nearer);

        /* Between the ends: the distance straight across to the line, when that is shorter. */
        if (*param > 0 && *param < 1) {
            const double orthogonal = vec_dot(normalized(vec_of(ab.y, -ab.x)), aq);

            if (fabs(orthogonal) < endpoint) {
                result.distance = orthogonal;
                result.dot      = 0;
                return result;
            }
        }

        /* Otherwise the distance to the nearer end, signed by the side of the line. */
        result.distance = nonzero_sign(vec_cross(aq, ab)) * endpoint;
        result.dot      = fabs(vec_dot(normalized(ab), normalized(nearer)));
        return result;
    }

    /* A curve: the nearest of its start, its end, and the points between that a cubic finds. */
    {
        /* The curve relative to the origin. */
        const font_vec qa = vec_minus(edge->p[0], origin);
        const font_vec ab = vec_minus(edge->p[1], edge->p[0]);
        const font_vec br = vec_minus(vec_minus(edge->p[2], edge->p[1]), ab);

        /* The cubic whose roots between 0 and 1 are where the nearest point may lie. */
        const double a = vec_dot(br, br);
        const double b = 3.0 * vec_dot(ab, br);
        const double c = 2.0 * vec_dot(ab, ab) + vec_dot(qa, br);
        const double d = vec_dot(qa, ab);
        double       roots[3];
        const int    count = solve_cubic(roots, a, b, c, d);

        /* The start is the nearest point so far. */
        font_vec direction = edge_direction(edge, 0);
        double   nearest   = nonzero_sign(vec_cross(direction, qa)) * vec_length(qa);

        *param = -vec_dot(qa, direction) / vec_dot(direction, direction);

        /* The end, if it is nearer. */
        {
            const font_vec end      = vec_minus(edge->p[2], origin);
            const double   distance = vec_length(end);

            if (distance < fabs(nearest)) {
                direction = edge_direction(edge, 1);
                nearest   = nonzero_sign(vec_cross(direction, end)) * distance;
                *param    = vec_dot(vec_minus(origin, edge->p[1]), direction) /
                    vec_dot(direction, direction);
            }
        }

        /* A point between the ends, if one is nearer still. */
        for (int at = 0; at < count; ++at) {
            const double t = roots[at];

            if (t > 0 && t < 1) {
                /* From the origin to the curve's point at t, and how far that is. */
                const font_vec qe = vec_plus(vec_plus(qa, vec_times(ab, 2.0 * t)),
                    vec_times(br, t * t));

                const double distance = vec_length(qe);

                if (distance <= fabs(nearest)) {
                    nearest = nonzero_sign(vec_cross(vec_plus(ab, vec_times(br, t)), qe)) *
                        distance;
                    *param = t;
                }
            }
        }

        result.distance = nearest;

        /* The tie-breaker: 0 when the nearest point is on the curve, else from the nearer end. */
        if (*param >= 0 && *param <= 1) {
            result.dot = 0;
        } else if (*param < 0.5) {
            result.dot = fabs(vec_dot(normalized(edge_direction(edge, 0)), normalized(qa)));
        } else {
            result.dot = fabs(vec_dot(normalized(edge_direction(edge, 1)),
                normalized(vec_minus(edge->p[2], origin))));
        }

        return result;
    }
}

/**
 * Beyond its ends an edge continues along its end tangents. That is what keeps a corner of the
 * median sharp, and it is only ever a shorter distance, never a longer one. Returns that distance
 * where it applies, else the signed distance as it was.
 */
static double pseudo_distance(const font_edge *edge, font_vec origin, signed_distance distance,
    double param) {
    if (param < 0) {
        /* Before the start: the distance to the line the start tangent runs along. */
        const font_vec direction = normalized(edge_direction(edge, 0));
        const font_vec aq        = vec_minus(origin, edge->p[0]);

        if (vec_dot(aq, direction) < 0) {
            const double pseudo = vec_cross(aq, direction);

            if (fabs(pseudo) <= fabs(distance.distance)) {
                return pseudo;
            }
        }
    } else if (param > 1) {
        /* Past the end: the distance to the line the end tangent runs along. */
        const font_vec direction = normalized(edge_direction(edge, 1));
        const font_vec bq        = vec_minus(origin, edge->p[2]);

        if (vec_dot(bq, direction) > 0) {
            const double pseudo = vec_cross(bq, direction);

            if (fabs(pseudo) <= fabs(distance.distance)) {
                return pseudo;
            }
        }
    }

    return distance.distance;
}

/** Unsigned distance to the nearest edge of any colour. */
static double true_distance(const font_shape *edges, font_vec origin) {
    double nearest = HUGE_VAL;

    for (size_t at = 0; at < edges->count; ++at) {
        double       param    = 0;
        const double distance = fabs(edge_distance(&edges->edges[at], origin, &param).distance);

        if (distance < nearest) {
            nearest = distance;
        }
    }

    return nearest;
}

/**
 * A signed distance as a byte of the field: 0.5 plus the distance over FONT_FIELD_RANGE, kept
 * within 0 to 1, then scaled to 0 to 255.
 */
static uint8_t encoded(double distance) {
    double value = 0.5 + distance / FONT_FIELD_RANGE;

    if (value < 0) {
        value = 0;
    }

    if (value > 1) {
        value = 1;
    }

    return (uint8_t)floor(value * 255.0 + 0.5);
}

/** The middle one of three bytes. */
static uint8_t median_of(uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t low = a < b ? a : b, high = a < b ? b : a;

    return c < low ? low : (c > high ? high : c);
}

/** The middle one of three values. */
static double median_float(double a, double b, double c) {
    return fmax(fmin(a, b), fmin(fmax(a, b), c));
}

/* --- One cell --------------------------------------------------------------------------------- */

/** The field of one cell while it is worked out, before it goes into the atlas. */
typedef struct cell_field {
    /** The computed window of the cell, in atlas texels. */
    int32_t  left;
    int32_t  top;
    uint32_t width;
    uint32_t height;
    /** Glyph space of a texel centre: x = texel + 0.5 - box_left + box_x, y mirrored. */
    double   box_left;
    double   box_top;
    int32_t  box_x;
    int32_t  box_y;
    /** Three channel bytes per texel, and each texel's true signed distance. */
    uint8_t *bytes;
    double  *truth;
    /** Per texel: 0 as computed, 1 newly settled on the true distance, 2 settled earlier. */
    uint8_t *disputed;
} cell_field;

/** The point of glyph space at atlas position (x, y). */
static font_vec glyph_point(const cell_field *field, double x, double y) {
    return vec_of(x - field->box_left + field->box_x, field->box_y - (y - field->box_top));
}

/** Settles a texel on its true distance: the same byte in all three channels. */
static void settle(cell_field *field, size_t texel) {
    const uint8_t value = encoded(field->truth[texel]);

    field->bytes[3u * texel] = field->bytes[3u * texel + 1u] = field->bytes[3u * texel + 2u] =
        value;
}

/**
 * Marks the four texels of a block when, at any of the sixteen points a 4x look at the block
 * samples, the interpolated median puts the edge on the wrong side of the outline. Returns whether
 * any texel was newly marked.
 */
static int check_block(cell_field *field, const font_shape *colored, const font_shape *shape,
    uint32_t u, uint32_t v) {
    /* The block's texels: top left, top right, bottom left, bottom right. */
    const size_t corners[4] = {
        (size_t)v * field->width + u, (size_t)v * field->width + u + 1u,
        ((size_t)v + 1u) * field->width + u, ((size_t)v + 1u) * field->width + u + 1u
    };
    int inside_any = 0, outside_any = 0;

    /* Whether any channel of the four texels says inside, and whether any says outside. */
    for (int corner = 0; corner < 4; ++corner) {
        for (int channel = 0; channel < 3; ++channel) {
            if (field->bytes[3u * corners[corner] + (size_t)channel] >= 128u) {
                inside_any = 1;
            } else {
                outside_any = 1;
            }
        }
    }

    /*
     * Every channel on one side at all four texels: no interpolation between them can cross over.
     */
    if (!(inside_any && outside_any)) {
        return 0;
    }

    /* The sixteen points, four by four, at the centres of a 4x grid over the block. */
    for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
            const double fx = 0.125 + 0.25 * sx, fy = 0.125 + 0.25 * sy;
            double       channels[3];
            int          inside_median, inside_outline;
            font_vec     point;

            /* Each channel interpolated between the four texels. */
            for (int channel = 0; channel < 3; ++channel) {
                const double top = field->bytes[3u * corners[0] + (size_t)channel] * (1.0 - fx) +
                    field->bytes[3u * corners[1] + (size_t)channel] * fx;
                const double bottom = field->bytes[3u * corners[2] + (size_t)channel] * (1.0 - fx) +
                    field->bytes[3u * corners[3] + (size_t)channel] * fx;

                channels[channel] = top * (1.0 - fy) + bottom * fy;
            }

            /* Inside or outside by the median of the channels, and by the outline. */
            inside_median = median_float(channels[0], channels[1], channels[2]) >= 127.5;

            point = glyph_point(field, field->left + u + 0.5 + fx, field->top + v + 0.5 + fy);

            inside_outline = font_shape_winding(shape, point) != 0;

            /* They disagree away from the outline: the four texels are settled. */
            if (inside_median != inside_outline && true_distance(colored, point) > TOLERANCE) {
                int marked = 0;

                for (int corner = 0; corner < 4; ++corner) {
                    if (!field->disputed[corners[corner]]) {
                        field->disputed[corners[corner]] = 1;
                        settle(field, corners[corner]);
                        marked = 1;
                    }
                }

                return marked;
            }
        }
    }

    return 0;
}

/**
 * Settles every texel whose own median is on the wrong side of the outline, away from it, then
 * checks the blocks of four texels pass after pass, until a pass settles no texel or the passes
 * run out.
 */
static void correct(cell_field *field, const font_shape *colored, const font_shape *shape) {
    const size_t texels = (size_t)field->width * field->height;
    uint8_t     *recheck;

    /* Each texel on its own: the side of its median against the sign of its true distance. */
    for (size_t texel = 0; texel < texels; ++texel) {
        const uint8_t median = median_of(field->bytes[3u * texel], field->bytes[3u * texel + 1u],
            field->bytes[3u * texel + 2u]);

        if ((median >= 128u) != (field->truth[texel] > 0) &&
            fabs(field->truth[texel]) > TOLERANCE) {
            field->disputed[texel] = 1;
            settle(field, texel);
        }
    }

    /* A field less than two texels wide or high has no block of four. */
    if (field->width < 2u || field->height < 2u) {
        return;
    }

    /* Which blocks the next pass checks, each block by its top-left texel. */
    recheck = malloc(texels);

    /* Without room to track which blocks changed, every pass simply checks them all. */
    if (recheck != NULL) {
        memset(recheck, 1, texels);
    }

    /* Then the blocks, pass after pass, until a pass settles nothing. */
    for (int pass = 0; pass < MOST_CORRECTION_PASSES; ++pass) {
        int changed = 0;

        for (uint32_t v = 0; v + 1u < field->height; ++v) {
            for (uint32_t u = 0; u + 1u < field->width; ++u) {
                const size_t block = (size_t)v * field->width + u;

                if (recheck != NULL && !recheck[block]) {
                    continue;
                }

                if (check_block(field, colored, shape, u, v)) {
                    changed = 1;
                }
            }
        }

        if (!changed) {
            break;
        }

        if (recheck != NULL) {
            /* A settled texel changes the four blocks it is a corner of. */
            memset(recheck, 0, texels);

            for (uint32_t v = 0; v < field->height; ++v) {
                for (uint32_t u = 0; u < field->width; ++u) {
                    /* Each newly settled texel queues its blocks once. */
                    if (field->disputed[(size_t)v * field->width + u] != 1) {
                        continue;
                    }

                    field->disputed[(size_t)v * field->width + u] = 2;

                    for (uint32_t dv = 0; dv < 2u; ++dv) {
                        for (uint32_t du = 0; du < 2u; ++du) {
                            if (u >= du && v >= dv) {
                                recheck[(size_t)(v - dv) * field->width + (u - du)] = 1;
                            }
                        }
                    }
                }
            }
        }
    }

    free(recheck);
}

/**
 * Colours the boundary, works out the field of the glyph's cell, corrects it, and writes it into
 * the red, green and blue of the cell's atlas pixels. An empty boundary leaves the cell as it is.
 */
edds_status font_field_render(
    font_shape           *boundary,
    const font_shape     *shape,
    const font_placement *placement,
    uint8_t              *atlas,
    uint32_t              atlas_width,
    edds_error           *error) {
    font_shape  colored;
    cell_field  field;
    edds_status status;
    int32_t     right, bottom;

    if (boundary->count == 0) {
        return EDDS_OK;
    }

    status = color_shape(boundary, &colored, error);

    if (status != EDDS_OK) {
        return status;
    }

    /* The box, centred in the cell. */
    memset(&field, 0, sizeof field);
    field.box_left = placement->cell_x + ((double)placement->cell - placement->width) * 0.5;
    field.box_top  = placement->cell_y + ((double)placement->cell - placement->height) * 0.5;
    field.box_x    = placement->box_x;
    field.box_y    = placement->box_y;

    /* The window of texels: the box and a margin around it, but never outside the cell. */
    field.left = (int32_t)floor(field.box_left - MARGIN);
    field.top  = (int32_t)floor(field.box_top - MARGIN);
    right      = (int32_t)ceil(field.box_left + placement->width + MARGIN);
    bottom     = (int32_t)ceil(field.box_top + placement->height + MARGIN);

    if (field.left < (int32_t)placement->cell_x) {
        field.left = (int32_t)placement->cell_x;
    }

    if (field.top < (int32_t)placement->cell_y) {
        field.top = (int32_t)placement->cell_y;
    }

    if (right > (int32_t)(placement->cell_x + placement->cell)) {
        right = (int32_t)(placement->cell_x + placement->cell);
    }

    if (bottom > (int32_t)(placement->cell_y + placement->cell)) {
        bottom = (int32_t)(placement->cell_y + placement->cell);
    }

    /* The window's size, and its buffers. */
    field.width    = (uint32_t)(right - field.left);
    field.height   = (uint32_t)(bottom - field.top);
    field.bytes    = malloc((size_t)field.width * field.height * 3u);
    field.truth    = malloc((size_t)field.width * field.height * sizeof *field.truth);
    field.disputed = calloc((size_t)field.width * field.height, 1);

    if (field.bytes == NULL || field.truth == NULL || field.disputed == NULL) {
        font_fail(error, "allocation-failed", "Memory for a glyph field could not be allocated.");
        status = EDDS_INTERNAL_FAILURE;
        goto done;
    }

    /* Each texel of the window: its three channels and its true distance. */
    for (uint32_t v = 0; v < field.height; ++v) {
        for (uint32_t u = 0; u < field.width; ++u) {
            /* The texel's centre, in glyph space. */
            const font_vec origin = glyph_point(&field, field.left + u + 0.5, field.top + v + 0.5);

            /* The nearest edge of each channel's colour so far, and the nearest of all. */
            signed_distance  best[3], nearest;
            double           params[3] = { 0, 0, 0 };
            const font_edge *owners[3] = { NULL, NULL, NULL };

            /* Where the texel's values are kept. */
            const size_t texel = (size_t)v * field.width + u;

            nearest.distance = HUGE_VAL;
            nearest.dot      = 0;

            for (int channel = 0; channel < 3; ++channel) {
                best[channel] = nearest;
            }

            /* Every edge, against the nearest of all and against the nearest of its channels. */
            for (size_t at = 0; at < colored.count; ++at) {
                const font_edge      *edge     = &colored.edges[at];
                double                param    = 0;
                const signed_distance distance = edge_distance(edge, origin, &param);

                if (fabs(distance.distance) < fabs(nearest.distance)) {
                    nearest = distance;
                }

                for (int channel = 0; channel < 3; ++channel) {
                    if ((edge->color & (1u << channel)) != 0 && closer(distance, best[channel])) {
                        best[channel]   = distance;
                        params[channel] = param;
                        owners[channel] = edge;
                    }
                }
            }

            /* Each channel's byte: the pseudo-distance to its nearest edge, or 0 without one. */
            for (int channel = 0; channel < 3; ++channel) {
                const double value = owners[channel] == NULL
                    ? -HUGE_VAL
                    : pseudo_distance(owners[channel], origin, best[channel], params[channel]);

                field.bytes[3u * texel + (size_t)channel] = encoded(value);
            }

            /* The true distance: to the nearest edge, positive where the outline winds. */
            field.truth[texel] = (font_shape_winding(shape, origin) != 0 ? 1.0 : -1.0) *
                fabs(nearest.distance);
        }
    }

    /* Where the median errs, the true distance instead. */
    correct(&field, &colored, shape);

    /* The field into the cell's atlas pixels, three bytes each; alpha stays as it is. */
    for (uint32_t v = 0; v < field.height; ++v) {
        uint8_t *row = atlas +
            ((size_t)(field.top + (int32_t)v) * atlas_width + (size_t)field.left) * 4u;

        for (uint32_t u = 0; u < field.width; ++u) {
            const size_t texel = (size_t)v * field.width + u;

            row[4u * u]      = field.bytes[3u * texel];
            row[4u * u + 1u] = field.bytes[3u * texel + 1u];
            row[4u * u + 2u] = field.bytes[3u * texel + 2u];
        }
    }

    status = EDDS_OK;

done:
    free(field.bytes);
    free(field.truth);
    free(field.disputed);
    font_shape_free(&colored);
    return status;
}
