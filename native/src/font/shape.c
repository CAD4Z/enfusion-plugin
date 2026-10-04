/*
 * Outline geometry in atlas pixels, y up. The union is what lets overlapping contours draw as one
 * shape: every edge is cut where any other edge crosses or runs along it, and a piece stays an edge
 * only if the area on one side of it is filled and on the other side is not.
 */
#include "geometry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Below this, two parameters or two points are the same one. */
#define SAME 1e-9

/** A piece shorter than this changes no pixel of any field and is dropped. */
#define SHORTEST_PIECE 1e-7

/** A pair of curves that cross more often than this is two copies of one curve. */
#define MOST_CROSSINGS 8

/** Whether two points are the same one: no further apart than SAME on either axis. */
static int same_point(font_vec a, font_vec b) {
    return fabs(a.x - b.x) <= SAME && fabs(a.y - b.y) <= SAME;
}

/** Frees the edges and loops of a shape and leaves it empty; a NULL shape is left alone. */
void font_shape_free(font_shape *shape) {
    if (shape == NULL) {
        return;
    }

    free(shape->edges);
    free(shape->loop_ends);
    memset(shape, 0, sizeof *shape);
}

/** Appends an edge to the open loop, doubling the room when it is full; 0 when memory runs out. */
int font_shape_push(font_shape *shape, const font_edge *edge) {
    if (shape->count == shape->capacity) {
        const size_t capacity = shape->capacity == 0 ? 32u : shape->capacity * 2u;
        font_edge   *grown    = realloc(shape->edges, capacity * sizeof *grown);

        if (grown == NULL) {
            return 0;
        }

        shape->edges    = grown;
        shape->capacity = capacity;
    }

    shape->edges[shape->count++] = *edge;

    return 1;
}

/**
 * Closes the loop of every edge pushed since the last one; an empty loop is not recorded. 0 when
 * memory runs out.
 */
static int end_loop(font_shape *shape) {
    /* The open loop starts where the last closed one ends. */
    const size_t start = shape->loop_count == 0 ? 0u : shape->loop_ends[shape->loop_count - 1u];

    if (shape->count == start) {
        return 1;
    }

    if (shape->loop_count == shape->loop_capacity) {
        const size_t capacity = shape->loop_capacity == 0 ? 8u : shape->loop_capacity * 2u;
        size_t      *grown    = realloc(shape->loop_ends, capacity * sizeof *grown);

        if (grown == NULL) {
            return 0;
        }

        shape->loop_ends     = grown;
        shape->loop_capacity = capacity;
    }

    shape->loop_ends[shape->loop_count++] = shape->count;

    return 1;
}

/** Reports that memory for a glyph shape ran out, and returns the status that goes with it. */
static edds_status out_of_memory(edds_error *error) {
    font_fail(error, "allocation-failed", "Memory for a glyph shape could not be allocated.");

    return EDDS_INTERNAL_FAILURE;
}

/**
 * A segment the outline really draws; a curve whose control point lies on its chord is a line. A
 * segment that draws nothing is left out. 0 when memory runs out.
 */
static int emit(font_shape *shape, font_vec from, const font_vec *control, font_vec to) {
    font_edge edge;

    /* A line from `from` to `to`, unless the control point makes it a curve below. */
    memset(&edge, 0, sizeof edge);
    edge.p[0] = from;
    edge.p[2] = to;
    edge.p[1] = vec_middle(from, to);

    if (control != NULL) {
        const font_vec chord        = vec_minus(to, from);
        const double   chord_length = vec_length(chord);
        const font_vec offset       = vec_minus(*control, from);

        /* Where the control point falls along the chord: 0 at `from`, 1 at `to`. */
        const double along = chord_length == 0
            ? -1.0
            : vec_dot(offset, chord) / (chord_length * chord_length);

        /* All three points in one place: nothing to draw. */
        if (chord_length == 0 && same_point(*control, from)) {
            return 1;
        }

        /* A curve, unless the control point lies on the chord, between its ends. */
        if (chord_length == 0 ||
            fabs(vec_cross(chord, offset)) > SAME * chord_length ||
            along < 0 ||
            along > 1) {
            edge.quad = 1;
            edge.p[1] = *control;
        }
    }

    /* A line of no length: nothing to draw. */
    if (!edge.quad && vec_length(vec_minus(to, from)) <= SAME) {
        return 1;
    }

    return font_shape_push(shape, &edge);
}

/**
 * TrueType contours as lines and quadratic segments, scaled, each contour one closed loop. The
 * caller frees `shape` with font_shape_free, whether or not this succeeds.
 */
edds_status font_shape_of_contours(
    const font_contours *contours,
    double               scale,
    font_shape          *shape,
    edds_error          *error) {
    size_t start = 0;

    memset(shape, 0, sizeof *shape);

    for (size_t contour = 0; contour < contours->contour_count; ++contour) {
        const size_t end   = contours->ends[contour];
        const size_t count = end - start;
        size_t       first = count;
        font_vec     current, opening, control = vec_of(0, 0);
        int          pending = 0;

        /* The first point on the curve; `count` when every point is a control point. */
        for (size_t at = 0; at < count; ++at) {
            if (contours->points[start + at].on_curve) {
                first = at;
                break;
            }
        }

        /* A contour of fewer than two points draws nothing. */
        if (count < 2u) {
            start = end;
            continue;
        }

        /* The contour's point at `index`, scaled to atlas pixels. */
#define POINT(index) \
    vec_of(contours->points[start + (index)].x *scale, contours->points[start + (index)].y *scale)

        /*
         * Round the contour, a control point waits in `control`, `pending` set, until the next
         * point says where its curve ends: at that point when it is on the curve, or halfway to it
         * when it is another control point. A point on the curve with none waiting ends a line.
         */
        if (first == count) {
            /*
             * Only control points: the contour starts halfway between the last one and the first.
             */
            opening = vec_middle(POINT(count - 1u), POINT(0));
            current = opening;

            for (size_t at = 0; at < count; ++at) {
                const font_vec next = POINT(at);

                if (pending) {
                    const font_vec middle = vec_middle(control, next);

                    if (!emit(shape, current, &control, middle)) {
                        return out_of_memory(error);
                    }

                    current = middle;
                }

                control = next;
                pending = 1;
            }
        } else {
            /* The contour starts at its first point on the curve. */
            opening = POINT(first);
            current = opening;

            for (size_t step = 1; step < count; ++step) {
                const size_t   at   = (first + step) % count;
                const font_vec next = POINT(at);

                if (contours->points[start + at].on_curve) {
                    if (!emit(shape, current, pending ? &control : NULL, next)) {
                        return out_of_memory(error);
                    }

                    current = next;
                    pending = 0;
                } else {
                    if (pending) {
                        const font_vec middle = vec_middle(control, next);

                        if (!emit(shape, current, &control, middle)) {
                            return out_of_memory(error);
                        }

                        current = middle;
                    }

                    control = next;
                    pending = 1;
                }
            }
        }
#undef POINT

        /* The last segment closes the contour at its opening point, and the loop ends. */
        if (!emit(shape, current, pending ? &control : NULL, opening) || !end_loop(shape)) {
            return out_of_memory(error);
        }

        start = end;
    }

    return EDDS_OK;
}

/* --- Winding ---------------------------------------------------------------------------------- */

/** The parameter in [from, to] where a y-monotonic quadratic reaches `y`. */
static double monotonic_root(const font_edge *edge, double y, double from, double to) {
    /* The curve's height less `y`, as a quadratic in the parameter, and its roots. */
    const double a = edge->p[0].y - 2.0 * edge->p[1].y + edge->p[2].y;
    const double b = 2.0 * (edge->p[1].y - edge->p[0].y);
    const double c = edge->p[0].y - y;
    double       roots[2];
    int          count = 0;

    /* The root nearest [from, to] so far, and how far outside it that root lies. */
    double best      = from;
    double best_miss = HUGE_VAL;

    /* A leading term this small: the equation is solved as a line. */
    if (fabs(a) < 1e-12) {
        if (b != 0) {
            roots[count++] = -c / b;
        }
    } else {
        double discriminant = b * b - 4.0 * a * c;
        double q;

        /* A negative discriminant is taken as 0. */
        if (discriminant < 0) {
            discriminant = 0;
        }

        q = -0.5 * (b + (b >= 0 ? sqrt(discriminant) : -sqrt(discriminant)));

        if (q != 0) {
            roots[count++] = q / a;
            roots[count++] = c / q;
        } else {
            roots[count++] = -b / (2.0 * a);
        }
    }

    /* The root inside [from, to], or the nearest to it, moved onto its nearer end. */
    for (int at = 0; at < count; ++at) {
        const double miss = roots[at] < from
            ? from - roots[at]
            : (roots[at] > to ? roots[at] - to : 0.0);

        if (miss < best_miss) {
            best_miss = miss;
            best      = roots[at] < from ? from : (roots[at] > to ? to : roots[at]);
        }
    }

    return best;
}

/**
 * Signed crossings of the ray from `point` towards +x, half-open in y so a shared vertex counts
 * once.
 */
static int crossings(const font_edge *edge, font_vec point) {
    int total = 0;

    /* A line crosses at most once: +1 going up, -1 going down. */
    if (!edge->quad) {
        const font_vec a = edge->p[0], b = edge->p[2];

        if ((a.y <= point.y && point.y < b.y) || (b.y <= point.y && point.y < a.y)) {
            const double x = a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);

            if (x > point.x) {
                total += a.y < b.y ? 1 : -1;
            }
        }

        return total;
    }

    /* A curve is cut where it turns in y; each of its pieces crosses at most once, like a line. */
    {
        double       cuts[3]     = { 0.0, 1.0, 1.0 };
        int          pieces      = 1;
        const double denominator = edge->p[0].y - 2.0 * edge->p[1].y + edge->p[2].y;

        if (denominator != 0) {
            const double extreme = (edge->p[0].y - edge->p[1].y) / denominator;

            if (extreme > 0 && extreme < 1) {
                cuts[1] = extreme;
                pieces  = 2;
            }
        }

        for (int piece = 0; piece < pieces; ++piece) {
            const double from = cuts[piece], to = cuts[piece + 1];
            const double y0 = from == 0 ? edge->p[0].y : edge_point(edge, from).y;
            const double y1 = to == 1 ? edge->p[2].y : edge_point(edge, to).y;

            if ((y0 <= point.y && point.y < y1) || (y1 <= point.y && point.y < y0)) {
                const double x = edge_point(edge, monotonic_root(edge, point.y, from, to)).x;

                if (x > point.x) {
                    total += y0 < y1 ? 1 : -1;
                }
            }
        }
    }

    return total;
}

/** The winding number of a point: the signed crossings of every edge with the ray from it. */
int font_shape_winding(const font_shape *shape, font_vec point) {
    int winding = 0;

    for (size_t at = 0; at < shape->count; ++at) {
        const font_edge *edge = &shape->edges[at];
        const double     low  = fmin(fmin(edge->p[0].y, edge->p[1].y), edge->p[2].y);
        const double     high = fmax(fmax(edge->p[0].y, edge->p[1].y), edge->p[2].y);

        /* An edge wholly above or below the point cannot cross its ray. */
        if (point.y < low || point.y >= high) {
            continue;
        }

        winding += crossings(edge, point);
    }

    return winding;
}

/** Exact extent of the curves, as left, bottom, right and top in `box`; 0 for an empty shape. */
int font_shape_bounds(const font_shape *shape, double box[4]) {
    if (shape->count == 0) {
        return 0;
    }

    /* The box starts at the first point and grows to take in every edge. */
    box[0] = box[2] = shape->edges[0].p[0].x;
    box[1] = box[3] = shape->edges[0].p[0].y;

    for (size_t at = 0; at < shape->count; ++at) {
        const font_edge *edge = &shape->edges[at];
        font_vec         points[4];
        int              count = 2;

        /* The ends of the edge, and where a curve turns in x and in y. */
        points[0] = edge->p[0];
        points[1] = edge->p[2];

        if (edge->quad) {
            const double dx = edge->p[0].x - 2.0 * edge->p[1].x + edge->p[2].x;
            const double dy = edge->p[0].y - 2.0 * edge->p[1].y + edge->p[2].y;

            if (dx != 0) {
                const double t = (edge->p[0].x - edge->p[1].x) / dx;

                if (t > 0 && t < 1) {
                    points[count++] = edge_point(edge, t);
                }
            }

            if (dy != 0) {
                const double t = (edge->p[0].y - edge->p[1].y) / dy;

                if (t > 0 && t < 1) {
                    points[count++] = edge_point(edge, t);
                }
            }
        }

        for (int point = 0; point < count; ++point) {
            if (points[point].x < box[0]) {
                box[0] = points[point].x;
            }

            if (points[point].y < box[1]) {
                box[1] = points[point].y;
            }

            if (points[point].x > box[2]) {
                box[2] = points[point].x;
            }

            if (points[point].y > box[3]) {
                box[3] = points[point].y;
            }
        }
    }

    return 1;
}

/* --- Intersections ---------------------------------------------------------------------------- */

/** A cut in an edge: the parameter it falls at, and the point there. */
typedef struct cut {
    double   t;
    font_vec at;
} cut;

/** The cuts found in one edge. */
typedef struct cut_list {
    cut   *items;
    size_t count;
    size_t capacity;
} cut_list;

/** What the search for crossings fills in: the cuts of every edge, and whether memory ran out. */
typedef struct union_state {
    cut_list *cuts;
    int       failed;
} union_state;

/**
 * Records a cut in an edge at parameter t; a cut at either end of the edge is no cut. When memory
 * runs out the cut is lost and `failed` is set.
 */
static void add_cut(union_state *state, size_t edge, double t, font_vec at) {
    cut_list *list = &state->cuts[edge];

    if (t <= SAME || t >= 1.0 - SAME) {
        return;
    }

    if (list->count == list->capacity) {
        const size_t capacity = list->capacity == 0 ? 4u : list->capacity * 2u;
        cut         *grown    = realloc(list->items, capacity * sizeof *grown);

        if (grown == NULL) {
            state->failed = 1;
            return;
        }

        list->items    = grown;
        list->capacity = capacity;
    }

    list->items[list->count].t  = t;
    list->items[list->count].at = at;
    ++list->count;
}

/** Whether a parameter falls on its edge: between 0 and 1, give or take SAME. */
static int within(double t) {
    return t >= -SAME && t <= 1.0 + SAME;
}

/** The parameter, kept within 0 to 1. */
static double clamp01(double t) {
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

/** Cuts two lines where they cross, or where a stretch they both run along starts and ends. */
static void line_line(union_state *state, const font_shape *shape, size_t first, size_t second) {
    const font_edge *a = &shape->edges[first], *b = &shape->edges[second];
    const font_vec   da = vec_minus(a->p[2], a->p[0]), db = vec_minus(b->p[2], b->p[0]);
    const font_vec   gap = vec_minus(b->p[0], a->p[0]);
    const double     la = vec_length(da), lb = vec_length(db);
    const double     denominator = vec_cross(da, db);

    /* Not parallel: the one point where the two lines meet, if it lies on both. */
    if (fabs(denominator) > 1e-12 * la * lb) {
        const double t = vec_cross(gap, db) / denominator;
        const double u = vec_cross(gap, da) / denominator;

        if (within(t) && within(u)) {
            const font_vec at = vec_plus(a->p[0], vec_times(da, clamp01(t)));

            add_cut(state, first, t, at);
            add_cut(state, second, u, at);
        }

        return;
    }

    /* Parallel: only a shared run matters, cut at each end of it. */
    if (fabs(vec_cross(gap, da)) > SAME * la) {
        return;
    }

    /* Each line is cut where the ends of the other one fall on it. */
    add_cut(state, first, vec_dot(vec_minus(b->p[0], a->p[0]), da) / (la * la), b->p[0]);
    add_cut(state, first, vec_dot(vec_minus(b->p[2], a->p[0]), da) / (la * la), b->p[2]);
    add_cut(state, second, vec_dot(vec_minus(a->p[0], b->p[0]), db) / (lb * lb), a->p[0]);
    add_cut(state, second, vec_dot(vec_minus(a->p[2], b->p[0]), db) / (lb * lb), a->p[2]);
}

/** Cuts a line and a curve where they cross. */
static void line_quad(union_state *state, const font_shape *shape, size_t line, size_t curve) {
    /* The two edges, and the direction of the line with its length squared. */
    const font_edge *a = &shape->edges[line], *b = &shape->edges[curve];
    const font_vec   direction = vec_minus(a->p[2], a->p[0]);
    const double     squared   = vec_dot(direction, direction);

    /* Each point of the curve measured across the line; the roots are where the curve meets it. */
    const double c0 = vec_cross(direction, vec_minus(b->p[0], a->p[0]));
    const double c1 = vec_cross(direction, vec_minus(b->p[1], a->p[0]));
    const double c2 = vec_cross(direction, vec_minus(b->p[2], a->p[0]));
    double       roots[2];
    const int    count = quadratic_roots(roots, c0 - 2.0 * c1 + c2, 2.0 * (c1 - c0), c0);

    /* A root on the curve whose point also lies on the line is a crossing. */
    for (int at = 0; at < count; ++at) {
        if (within(roots[at])) {
            const double   u     = clamp01(roots[at]);
            const font_vec point = edge_point(b, u);
            const double   t     = vec_dot(vec_minus(point, a->p[0]), direction) / squared;

            if (within(t)) {
                add_cut(state, line, t, point);
                add_cut(state, curve, u, point);
            }
        }
    }
}

/** A piece of a curve in the crossing search: its three points, and the parameters it spans. */
typedef struct piece {
    font_vec p[3];
    double   from;
    double   to;
} piece;

/** Splits a piece into its first and its second half. */
static void split_piece(const piece *whole, piece *left, piece *right) {
    const font_vec a      = vec_middle(whole->p[0], whole->p[1]);
    const font_vec b      = vec_middle(whole->p[1], whole->p[2]);
    const font_vec middle = vec_middle(a, b);
    const double   half   = 0.5 * (whole->from + whole->to);

    left->p[0] = whole->p[0];
    left->p[1] = a;
    left->p[2] = middle;
    left->from = whole->from;
    left->to   = half;

    right->p[0] = middle;
    right->p[1] = b;
    right->p[2] = whole->p[2];
    right->from = half;
    right->to   = whole->to;
}

/** Whether the boxes around the points of two pieces meet, the first box widened by SAME. */
static int boxes_meet(const piece *a, const piece *b) {
    /* The box of the first piece, widened by SAME on every side. */
    const double ax0 = fmin(fmin(a->p[0].x, a->p[1].x), a->p[2].x) - SAME;
    const double ax1 = fmax(fmax(a->p[0].x, a->p[1].x), a->p[2].x) + SAME;
    const double ay0 = fmin(fmin(a->p[0].y, a->p[1].y), a->p[2].y) - SAME;
    const double ay1 = fmax(fmax(a->p[0].y, a->p[1].y), a->p[2].y) + SAME;

    /* The box of the second piece. */
    const double bx0 = fmin(fmin(b->p[0].x, b->p[1].x), b->p[2].x);
    const double bx1 = fmax(fmax(b->p[0].x, b->p[1].x), b->p[2].x);
    const double by0 = fmin(fmin(b->p[0].y, b->p[1].y), b->p[2].y);
    const double by1 = fmax(fmax(b->p[0].y, b->p[1].y), b->p[2].y);

    return ax0 <= bx1 && bx0 <= ax1 && ay0 <= by1 && by0 <= ay1;
}

/** How far the control point of a piece lies off its chord; off its start when it has no chord. */
static double flatness(const piece *part) {
    const font_vec chord        = vec_minus(part->p[2], part->p[0]);
    const double   chord_length = vec_length(chord);
    const font_vec offset       = vec_minus(part->p[1], part->p[0]);

    return chord_length == 0 ? vec_length(offset) : fabs(vec_cross(chord, offset)) / chord_length;
}

/** Subdivision steps one pair of curves may take; near-coincident curves would never stop. */
#define MOST_SEARCH_STEPS 4096

/** The search for crossings between two curves. */
typedef struct quad_search {
    union_state     *state;
    /** The two curves, and where they are in the shape. */
    const font_edge *a;
    const font_edge *b;
    size_t           first;
    size_t           second;
    /** The crossings found so far, by their parameter on the first curve. */
    double           found[MOST_CROSSINGS];
    int              found_count;
    /** The subdivision steps taken so far. */
    int              steps;
} quad_search;

/** Newton on A(t) = B(u) from the chord estimate; keeps the estimate if it does not converge. */
static void refine(const font_edge *a, const font_edge *b, double *t, double *u) {
    double t1 = *t, u1 = *u;

    /* At most eight steps. */
    for (int step = 0; step < 8; ++step) {
        /* The gap between the two points, and the direction of each curve there. */
        const font_vec gap = vec_minus(edge_point(a, t1), edge_point(b, u1));
        const font_vec da  = vec_times(edge_direction(a, t1), 2.0),
                       db  = vec_times(edge_direction(b, u1), 2.0);

        /* The step to take on each curve, and the determinant it is solved with. */
        const double determinant = vec_cross(da, vec_times(db, -1.0));
        double       dt, du;

        /* Parallel directions: no step can be worked out. */
        if (fabs(determinant) < 1e-18) {
            return;
        }

        /* [da -db] [dt du]^T = -gap */
        dt  = vec_cross(vec_times(gap, -1.0), vec_times(db, -1.0)) / determinant;
        du  = vec_cross(da, vec_times(gap, -1.0)) / determinant;
        t1 += dt;
        u1 += du;

        /* A step off either curve leaves the estimate as it was. */
        if (!within(t1) || !within(u1)) {
            return;
        }

        if (fabs(dt) < 1e-14 && fabs(du) < 1e-14) {
            break;
        }
    }

    /* Only parameters at which the two curves meet replace the estimate. */
    if (vec_length(vec_minus(edge_point(a, t1), edge_point(b, u1))) < 1e-9) {
        *t = clamp01(t1);
        *u = clamp01(u1);
    }
}

/**
 * Looks for crossings between two pieces of the two curves. Pieces whose boxes meet are halved,
 * and halved again, until both are flat or 48 halvings deep; then they cross as straight lines, if
 * at all. The search stops at MOST_CROSSINGS crossings, or after MOST_SEARCH_STEPS steps.
 */
static void quad_quad_search(quad_search *search, const piece *a, const piece *b, int depth) {
    if (search->found_count >= MOST_CROSSINGS ||
        search->state->failed ||
        ++search->steps > MOST_SEARCH_STEPS ||
        !boxes_meet(a, b)) {
        return;
    }

    /* Both flat, or deep enough: where their chords cross is a crossing of the curves. */
    if ((flatness(a) < 1e-7 && flatness(b) < 1e-7) || depth >= 48) {
        const font_vec da = vec_minus(a->p[2], a->p[0]), db = vec_minus(b->p[2], b->p[0]);
        const double   denominator = vec_cross(da, db);

        if (fabs(denominator) > 1e-18) {
            const font_vec gap = vec_minus(b->p[0], a->p[0]);

            /* How far along each chord they cross, from 0 at its start to 1 at its end. */
            const double s = vec_cross(gap, db) / denominator, v = vec_cross(gap, da) / denominator;

            if (s >= -1e-6 && s <= 1 + 1e-6 && v >= -1e-6 && v <= 1 + 1e-6) {
                double   t = a->from + clamp01(s) * (a->to - a->from);
                double   u = b->from + clamp01(v) * (b->to - b->from);
                font_vec at;

                /* The crossing on the whole curves, refined, and the point between their two. */
                refine(search->a, search->b, &t, &u);
                at = vec_middle(edge_point(search->a, t), edge_point(search->b, u));

                /* Neighbouring pieces of one crossing find it again; it is still one crossing. */
                for (int seen = 0; seen < search->found_count; ++seen) {
                    if (fabs(search->found[seen] - t) <= 1e-7) {
                        return;
                    }
                }

                add_cut(search->state, search->first, t, at);
                add_cut(search->state, search->second, u, at);
                search->found[search->found_count++] = t;
            }
        }

        return;
    }

    /* Otherwise both pieces are halved, and every half searched against every half of the other. */
    {
        piece a0, a1, b0, b1;

        split_piece(a, &a0, &a1);
        split_piece(b, &b0, &b1);
        quad_quad_search(search, &a0, &b0, depth + 1);
        quad_quad_search(search, &a0, &b1, depth + 1);
        quad_quad_search(search, &a1, &b0, depth + 1);
        quad_quad_search(search, &a1, &b1, depth + 1);
    }
}

/** Whether two curves are one and the same, run either way. */
static int same_curve(const font_edge *a, const font_edge *b) {
    return same_point(a->p[1], b->p[1]) &&
        ((same_point(a->p[0], b->p[0]) && same_point(a->p[2], b->p[2])) ||
            (same_point(a->p[0], b->p[2]) && same_point(a->p[2], b->p[0])));
}

/** Cuts two curves where they cross; a curve and its own copy are left alone. */
static void quad_quad(union_state *state, const font_shape *shape, size_t first, size_t second) {
    const font_edge *a = &shape->edges[first], *b = &shape->edges[second];
    quad_search      search;
    piece            whole_a, whole_b;

    if (same_curve(a, b)) {
        return;
    }

    memset(&search, 0, sizeof search);
    search.state  = state;
    search.a      = a;
    search.b      = b;
    search.first  = first;
    search.second = second;

    /* The search starts from the two whole curves. */
    memcpy(whole_a.p, a->p, sizeof whole_a.p);
    memcpy(whole_b.p, b->p, sizeof whole_b.p);
    whole_a.from = whole_b.from = 0;
    whole_a.to = whole_b.to = 1;
    quad_quad_search(&search, &whole_a, &whole_b, 0);
}

/** The box around an edge's three points, widened by SAME: left, bottom, right and top. */
static void edge_box(const font_edge *edge, double box[4]) {
    box[0] = fmin(fmin(edge->p[0].x, edge->p[1].x), edge->p[2].x) - SAME;
    box[1] = fmin(fmin(edge->p[0].y, edge->p[1].y), edge->p[2].y) - SAME;
    box[2] = fmax(fmax(edge->p[0].x, edge->p[1].x), edge->p[2].x) + SAME;
    box[3] = fmax(fmax(edge->p[0].y, edge->p[1].y), edge->p[2].y) + SAME;
}

/* --- The union -------------------------------------------------------------------------------- */

/** The qsort comparison for cuts in ascending order of parameter: -1, 0 or 1. */
static int ascending_cut(const void *left, const void *right) {
    const double a = ((const cut *)left)->t, b = ((const cut *)right)->t;

    return (a > b) - (a < b);
}

/** The part of `edge` between parameters `from` and `to`, ending exactly at the given points. */
static font_edge sub_edge(const font_edge *edge, double from, double to, font_vec start,
    font_vec end) {
    font_edge result = edge_part(edge, from, to);

    result.p[0] = start;
    result.p[2] = end;

    /* A line keeps its middle point halfway between its new ends. */
    if (!result.quad) {
        result.p[1] = vec_middle(start, end);
    }

    return result;
}

/** Turns an edge round: its start becomes its end. */
static void reverse(font_edge *edge) {
    const font_vec start = edge->p[0];

    edge->p[0] = edge->p[2];
    edge->p[2] = start;
}

/**
 * The length of an edge; for a curve, roughly: the mean of its chord and the way through its
 * control point.
 */
static double rough_length(const font_edge *edge) {
    if (!edge->quad) {
        return vec_length(vec_minus(edge->p[2], edge->p[0]));
    }

    return 0.5 *
        (vec_length(vec_minus(edge->p[2], edge->p[0])) +
            vec_length(vec_minus(edge->p[1], edge->p[0])) +
            vec_length(vec_minus(edge->p[2], edge->p[1])));
}

/**
 * Filled on exactly one side: a piece of the union's boundary, turned so the fill is on its right.
 * Returns 1 for such a piece, and 0 for any other.
 */
static int keep_as_boundary(const font_shape *shape, font_edge *edge) {
    const font_vec middle  = edge_point(edge, 0.5);
    font_vec       tangent = edge_direction(edge, 0.5);
    double         size    = vec_length(tangent);
    double         step    = rough_length(edge) * 1e-3;
    font_vec       right;
    int            filled_right, filled_left;

    if (size == 0) {
        return 0;
    }

    /* The direction to the right of the edge, at its middle. */
    tangent = vec_times(tangent, 1.0 / size);
    right   = vec_of(tangent.y, -tangent.x);

    /* How far to each side to look: a thousandth of the edge, kept between 1e-8 and 1e-4. */
    if (step > 1e-4) {
        step = 1e-4;
    }

    if (step < 1e-8) {
        step = 1e-8;
    }

    /* Whether the outline fills the point that far to the right of the middle, and to the left. */
    filled_right = font_shape_winding(shape, vec_plus(middle, vec_times(right, step))) != 0;
    filled_left  = font_shape_winding(shape, vec_minus(middle, vec_times(right, step))) != 0;

    if (filled_right == filled_left) {
        return 0;
    }

    if (!filled_right) {
        reverse(edge);
    }

    return 1;
}

/** Whether two edges are the same, running the same way. */
static int same_edge(const font_edge *a, const font_edge *b) {
    return a->quad == b->quad &&
        same_point(a->p[0], b->p[0]) &&
        same_point(a->p[2], b->p[2]) &&
        (!a->quad || same_point(a->p[1], b->p[1]));
}

/** Pieces ordered by where they start, so the piece starting at a point is a short scan away. */
typedef struct start_key {
    double x;
    double y;
    size_t index;
} start_key;

/** The qsort order of start keys: by x, then by y. */
static int by_start(const void *left, const void *right) {
    const start_key *a = left, *b = right;

    if (a->x != b->x) {
        return a->x < b->x ? -1 : 1;
    }

    return (a->y > b->y) - (a->y < b->y);
}

/** First position in `keys` whose piece starts at x >= `x`. */
static size_t first_from(const start_key *keys, size_t count, double x) {
    size_t low = 0, high = count;

    /* The answer lies between low and high; the gap between them is halved until it closes. */
    while (low < high) {
        const size_t middle = low + (high - low) / 2u;

        if (keys[middle].x < x) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }

    return low;
}

/**
 * The boundary of the area the shape fills: every edge is cut where another crosses it, the pieces
 * filled on one side only are kept, a second copy of a piece is dropped, and the rest are chained
 * into loops. On success the caller owns `boundary` and frees it with font_shape_free; on failure
 * it is left empty.
 */
edds_status font_shape_union(const font_shape *shape, font_shape *boundary, edds_error *error) {
    /* The cuts in every edge, and the pieces kept as boundary. */
    union_state state  = { NULL, 0 };
    font_shape  pieces = { 0 };

    /* The box around every edge. */
    double (*boxes)[4] = NULL;

    /* Which pieces are taken, and the pieces ordered by where they start. */
    unsigned char *used  = NULL;
    start_key     *order = NULL;

    edds_status status = EDDS_OK;

    memset(boundary, 0, sizeof *boundary);

    if (shape->count == 0) {
        return EDDS_OK;
    }

    state.cuts = calloc(shape->count, sizeof *state.cuts);
    boxes      = malloc(shape->count * sizeof *boxes);

    if (state.cuts == NULL || boxes == NULL) {
        status = out_of_memory(error);
        goto done;
    }

    for (size_t at = 0; at < shape->count; ++at) {
        edge_box(&shape->edges[at], boxes[at]);
    }

    /* Every two edges whose boxes meet are cut where they cross: lines, curves, or one of each. */
    for (size_t first = 0; first < shape->count && !state.failed; ++first) {
        for (size_t second = first + 1u; second < shape->count && !state.failed; ++second) {
            const font_edge *a = &shape->edges[first], *b = &shape->edges[second];

            if (boxes[first][0] > boxes[second][2] ||
                boxes[second][0] > boxes[first][2] ||
                boxes[first][1] > boxes[second][3] ||
                boxes[second][1] > boxes[first][3]) {
                continue;
            }

            if (!a->quad && !b->quad) {
                line_line(&state, shape, first, second);
            } else if (!a->quad) {
                line_quad(&state, shape, first, second);
            } else if (!b->quad) {
                line_quad(&state, shape, second, first);
            } else {
                quad_quad(&state, shape, first, second);
            }
        }
    }

    if (state.failed) {
        status = out_of_memory(error);
        goto done;
    }

    /* Cut every edge into pieces and keep the ones on the boundary of the filled area. */
    for (size_t at = 0; at < shape->count; ++at) {
        const font_edge *edge  = &shape->edges[at];
        cut_list        *list  = &state.cuts[at];
        double           from  = 0;
        font_vec         start = edge->p[0];

        qsort(list->items, list->count, sizeof *list->items, ascending_cut);

        /* From cut to cut, the last piece running on to the end of the edge. */
        for (size_t index = 0; index <= list->count; ++index) {
            const double   to  = index == list->count ? 1.0 : list->items[index].t;
            const font_vec end = index == list->count ? edge->p[2] : list->items[index].at;
            font_edge      part;

            if (to - from <= SAME) {
                continue;
            }

            part  = sub_edge(edge, from, to, start, end);
            from  = to;
            start = end;

            /* A piece too short to matter, or not on the boundary, is dropped. */
            if (rough_length(&part) <= SHORTEST_PIECE || !keep_as_boundary(shape, &part)) {
                continue;
            }

            if (!font_shape_push(&pieces, &part)) {
                status = out_of_memory(error);
                goto done;
            }
        }
    }

    if (pieces.count == 0) {
        goto done;
    }

    /* The pieces ordered by where they start; none taken yet. */
    order = malloc(pieces.count * sizeof *order);
    used  = calloc(pieces.count, 1);

    if (order == NULL || used == NULL) {
        status = out_of_memory(error);
        goto done;
    }

    for (size_t at = 0; at < pieces.count; ++at) {
        order[at].x     = pieces.edges[at].p[0].x;
        order[at].y     = pieces.edges[at].p[0].y;
        order[at].index = at;
    }

    qsort(order, pieces.count, sizeof *order, by_start);

    /* Two contours that share a stretch both see it as boundary: one copy is the edge. */
    for (size_t at = 0; at < pieces.count; ++at) {
        const font_edge *piece_at = &pieces.edges[order[at].index];

        if (used[order[at].index]) {
            continue;
        }

        /* Later copies among the pieces starting at about the same x are marked as taken. */
        for (size_t later = at + 1u;
            later < pieces.count && order[later].x <= order[at].x + SAME; ++later) {
            if (same_edge(piece_at, &pieces.edges[order[later].index])) {
                used[order[later].index] = 1;
            }
        }
    }

    /* Chain the pieces end to start into closed loops. */
    for (size_t seed = 0; seed < pieces.count; ++seed) {
        size_t         current = seed;
        const font_vec opening = pieces.edges[seed].p[0];

        if (used[seed]) {
            continue;
        }

        for (;;) {
            font_edge edge    = pieces.edges[current];
            size_t    next    = pieces.count;
            double    nearest = 1e-6;

            used[current] = 1;

            /* Each piece after the first starts exactly where the one before it ends. */
            if (current != seed) {
                edge.p[0] = boundary->edges[boundary->count - 1u].p[2];
            }

            if (!font_shape_push(boundary, &edge)) {
                status = out_of_memory(error);
                goto done;
            }

            /* Back at the opening point: the loop is closed. */
            if (same_point(edge.p[2], opening)) {
                break;
            }

            /* The next piece: the untaken one whose start is nearest this end, within 1e-6. */
            for (size_t at = first_from(order, pieces.count, edge.p[2].x - nearest);
                at < pieces.count && order[at].x <= edge.p[2].x + nearest; ++at) {
                const double distance = vec_length(
                    vec_minus(pieces.edges[order[at].index].p[0], edge.p[2]));

                if (!used[order[at].index] && distance < nearest) {
                    nearest = distance;
                    next    = order[at].index;
                }
            }

            /* No piece to go on with: the loop ends here. */
            if (next == pieces.count) {
                break;
            }

            current = next;
        }

        if (!end_loop(boundary)) {
            status = out_of_memory(error);
            goto done;
        }
    }

done:
    if (state.cuts != NULL) {
        for (size_t at = 0; at < shape->count; ++at) {
            free(state.cuts[at].items);
        }
    }

    free(state.cuts);
    free(boxes);
    free(used);
    free(order);
    font_shape_free(&pieces);

    /* A failed union leaves no boundary behind. */
    if (status != EDDS_OK) {
        font_shape_free(boundary);
    }

    return status;
}
