/*
 * Points and edges in atlas pixels, y up, and the few operations on them that both the outline
 * union and the distance field are built from.
 */
#ifndef FONT_GEOMETRY_H
#define FONT_GEOMETRY_H

#include "font_internal.h"

#include <math.h>

static inline font_vec vec_of(double x, double y) {
    font_vec result;
    result.x = x;
    result.y = y;
    return result;
}

static inline font_vec vec_plus(font_vec a, font_vec b) {
    return vec_of(a.x + b.x, a.y + b.y);
}

static inline font_vec vec_minus(font_vec a, font_vec b) {
    return vec_of(a.x - b.x, a.y - b.y);
}

static inline font_vec vec_times(font_vec a, double factor) {
    return vec_of(a.x * factor, a.y * factor);
}

static inline double vec_dot(font_vec a, font_vec b) {
    return a.x * b.x + a.y * b.y;
}

static inline double vec_cross(font_vec a, font_vec b) {
    return a.x * b.y - a.y * b.x;
}

static inline double vec_length(font_vec a) {
    return sqrt(vec_dot(a, a));
}

static inline font_vec vec_middle(font_vec a, font_vec b) {
    return vec_of(0.5 * (a.x + b.x), 0.5 * (a.y + b.y));
}

static inline font_vec edge_point(const font_edge *edge, double t) {
    if (!edge->quad) {
        return vec_plus(edge->p[0], vec_times(vec_minus(edge->p[2], edge->p[0]), t));
    }
    {
        const double s = 1.0 - t;
        return vec_of(s * s * edge->p[0].x + 2.0 * s * t * edge->p[1].x + t * t * edge->p[2].x,
            s * s * edge->p[0].y + 2.0 * s * t * edge->p[1].y + t * t * edge->p[2].y);
    }
}

/** The tangent, never zero: a curve whose control point sits on an end falls back to its chord. */
static inline font_vec edge_direction(const font_edge *edge, double t) {
    if (edge->quad) {
        const font_vec tangent = vec_plus(vec_times(vec_minus(edge->p[1], edge->p[0]), 1.0 - t),
            vec_times(vec_minus(edge->p[2], edge->p[1]), t));
        if (tangent.x != 0 || tangent.y != 0) {
            return tangent;
        }
    }
    return vec_minus(edge->p[2], edge->p[0]);
}

/** The part of an edge between two parameters; a curve's control point is its blossom at both. */
static inline font_edge edge_part(const font_edge *edge, double from, double to) {
    font_edge result = *edge;
    result.p[0]      = edge_point(edge, from);
    result.p[2]      = edge_point(edge, to);
    if (edge->quad) {
        const double a = (1.0 - from) * (1.0 - to), b = (1.0 - from) * to + from * (1.0 - to), c = from * to;
        result.p[1] = vec_of(a * edge->p[0].x + b * edge->p[1].x + c * edge->p[2].x,
            a * edge->p[0].y + b * edge->p[1].y + c * edge->p[2].y);
    } else {
        result.p[1] = vec_middle(result.p[0], result.p[2]);
    }
    return result;
}

/**
 * Real roots of a·x² + b·x + c in the cancellation-free form. A leading term this small next to
 * the linear one is noise, and the equation is solved as the line it really is.
 */
static inline int quadratic_roots(double roots[2], double a, double b, double c) {
    double discriminant, q;
    if (a == 0 || fabs(b) > 1e12 * fabs(a)) {
        if (b == 0) {
            return 0;
        }
        roots[0] = -c / b;
        return 1;
    }
    discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0) {
        return 0;
    }
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
