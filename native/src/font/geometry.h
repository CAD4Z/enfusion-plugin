/*
 * Points and edges in atlas pixels, y up, and the few operations on them that both the outline
 * union and the distance field are built from.
 */
#ifndef FONT_GEOMETRY_H
#define FONT_GEOMETRY_H

#include "font_internal.h"

#include <math.h>

/** The vector (x, y). */
static inline font_vec vec_of(double x, double y) {
    font_vec result;

    result.x = x;
    result.y = y;

    return result;
}

/** The sum a + b. */
static inline font_vec vec_plus(font_vec a, font_vec b) {
    return vec_of(a.x + b.x, a.y + b.y);
}

/** The difference a - b. */
static inline font_vec vec_minus(font_vec a, font_vec b) {
    return vec_of(a.x - b.x, a.y - b.y);
}

/** The vector a scaled by a factor. */
static inline font_vec vec_times(font_vec a, double factor) {
    return vec_of(a.x * factor, a.y * factor);
}

/** The dot product of a and b. */
static inline double vec_dot(font_vec a, font_vec b) {
    return a.x * b.x + a.y * b.y;
}

/** The two-dimensional cross product of a and b. */
static inline double vec_cross(font_vec a, font_vec b) {
    return a.x * b.y - a.y * b.x;
}

/** The length of a. */
static inline double vec_length(font_vec a) {
    return sqrt(vec_dot(a, a));
}

/** The point halfway between a and b. */
static inline font_vec vec_middle(font_vec a, font_vec b) {
    return vec_of(0.5 * (a.x + b.x), 0.5 * (a.y + b.y));
}

/** The point of an edge at parameter t: p[0] at 0, p[2] at 1. */
static inline font_vec edge_point(const font_edge *edge, double t) {
    if (!edge->quad) {
        /* A line: t of the way from p[0] to p[2]. */
        return vec_plus(edge->p[0], vec_times(vec_minus(edge->p[2], edge->p[0]), t));
    }

    /* A curve: the point of the quadratic Bezier with control point p[1]. */
    {
        const double s = 1.0 - t;

        return vec_of(s * s * edge->p[0].x + 2.0 * s * t * edge->p[1].x + t * t * edge->p[2].x,
            s * s * edge->p[0].y + 2.0 * s * t * edge->p[1].y + t * t * edge->p[2].y);
    }
}

/** The tangent, never zero: a curve whose control point sits on an end falls back to its chord. */
static inline font_vec edge_direction(const font_edge *edge, double t) {
    if (edge->quad) {
        /* A curve: its two legs, p[0] to p[1] and p[1] to p[2], blended by t. */
        const font_vec tangent = vec_plus(vec_times(vec_minus(edge->p[1], edge->p[0]), 1.0 - t),
            vec_times(vec_minus(edge->p[2], edge->p[1]), t));

        if (tangent.x != 0 || tangent.y != 0) {
            return tangent;
        }
    }

    /* The chord from p[0] to p[2]: a line's direction, and the fallback for a curve. */
    return vec_minus(edge->p[2], edge->p[0]);
}

/** The part of an edge between two parameters; a curve's control point is its blossom at both. */
static inline font_edge edge_part(const font_edge *edge, double from, double to) {
    font_edge result = *edge;

    /* The new ends: the edge's own points at the two parameters. */
    result.p[0] = edge_point(edge, from);
    result.p[2] = edge_point(edge, to);

    /* The new control point: the blossom of a curve, the middle of a line. */
    if (edge->quad) {
        const double a = (1.0 - from) * (1.0 - to),
                     b = (1.0 - from) * to + from * (1.0 - to),
                     c = from * to;

        result.p[1] = vec_of(a * edge->p[0].x + b * edge->p[1].x + c * edge->p[2].x,
            a * edge->p[0].y + b * edge->p[1].y + c * edge->p[2].y);
    } else {
        result.p[1] = vec_middle(result.p[0], result.p[2]);
    }

    return result;
}

/**
 * Real roots of a*x^2 + b*x + c in the cancellation-free form. A leading term this small next to
 * the linear one is noise, and the equation is solved as the line it really is. The roots go to
 * `roots`, and the return value says how many there are: 0, 1 or 2.
 */
static inline int quadratic_roots(double roots[2], double a, double b, double c) {
    double discriminant, q;

    /* The line b*x + c: one root, or none when b is 0 as well. */
    if (a == 0 || fabs(b) > 1e12 * fabs(a)) {
        if (b == 0) {
            return 0;
        }

        roots[0] = -c / b;
        return 1;
    }

    /* A negative discriminant: no real root. */
    discriminant = b * b - 4.0 * a * c;

    if (discriminant < 0) {
        return 0;
    }

    /* The roots are q / a and c / q, with q taking the sign of b so the two terms never cancel. */
    q = -0.5 * (b + (b >= 0 ? sqrt(discriminant) : -sqrt(discriminant)));

    if (q == 0) {
        roots[0] = -b / (2.0 * a);
        return 1;
    }

    roots[0] = q / a;
    roots[1] = c / q;

    return 2;
}

#endif
