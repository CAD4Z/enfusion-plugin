#include "font_internal.h"

#include <edds/pool.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * One font from one TrueType file: every requested character that the font has, plus the space and
 * the missing-glyph box the engine needs, each in a square cell of one atlas no larger than 4096.
 */

/* The engine draws a box plus 5 atlas pixels on every side, so a cell is the largest box plus 10. */
#define CELL_MARGIN 10u

/* The box a generator draws for a font that lacks U+25A1, and the space it makes up, in em. */
static const double frame_left = 0.06, frame_bottom = -0.06, frame_side = 0.66, frame_stroke = 0.07;
static const double frame_advance = 0.78, made_up_space = 0.25;

typedef enum glyph_origin {
    FROM_FONT,
    DRAWN_SPACE,
    DRAWN_BOX
} glyph_origin;

/** One cell of the atlas: a glyph of the font, or one the generator drew. */
typedef struct glyph_job {
    glyph_origin origin;
    uint32_t glyph;
    font_shape shape;
    font_shape boundary;
    int32_t box_x;
    int32_t box_y;
    uint32_t width;
    uint32_t height;
    int32_t advance;
    font_placement placement;
    edds_status status;
    edds_error error;
} glyph_job;

typedef struct character {
    uint32_t code;
    uint32_t glyph;
    size_t job;
} character;

typedef struct render_run {
    glyph_job *jobs;
    size_t count;
    uint8_t *atlas;
    uint32_t atlas_width;
    edds_cancelled_fn cancelled;
    void *cancel_context;
    edds_progress_fn progress;
    void *progress_context;
    size_t finished;
} render_run;

static double rounded(double value) {
    return value >= 0 ? floor(value + 0.5) : -floor(-value + 0.5);
}

static int add_rectangle(font_contours *contours, double x0, double y0, double x1, double y1, int clockwise) {
    const size_t base = contours->count;
    font_point *points;
    size_t *ends;
    if (contours->count + 4u > contours->capacity) {
        const size_t capacity = contours->capacity + 8u;
        points = realloc(contours->points, capacity * sizeof *points);
        if (points == NULL) return 0;
        contours->points = points;
        contours->capacity = capacity;
    }
    if (contours->contour_count == contours->contour_capacity) {
        const size_t capacity = contours->contour_capacity + 4u;
        ends = realloc(contours->ends, capacity * sizeof *ends);
        if (ends == NULL) return 0;
        contours->ends = ends;
        contours->contour_capacity = capacity;
    }
    points = contours->points + base;
    points[0].x = x0;
    points[0].y = y0;
    points[1].x = clockwise ? x0 : x1;
    points[1].y = clockwise ? y1 : y0;
    points[2].x = x1;
    points[2].y = y1;
    points[3].x = clockwise ? x1 : x0;
    points[3].y = clockwise ? y0 : y1;
    for (int at = 0; at < 4; ++at) points[at].on_curve = 1;
    contours->count += 4u;
    contours->ends[contours->contour_count++] = contours->count;
    return 1;
}

static edds_status job_contours(const font_face *face, glyph_job *job, font_contours *contours,
    uint32_t *advance, edds_error *error) {
    const double em = face->units_per_em;
    memset(contours, 0, sizeof *contours);
    if (job->origin == FROM_FONT) return font_face_contours(face, job->glyph, contours, advance, error);
    if (job->origin == DRAWN_SPACE) {
        *advance = (uint32_t)rounded(made_up_space * em);
        return EDDS_OK;
    }
    *advance = (uint32_t)rounded(frame_advance * em);
    {
        const double x0 = frame_left * em, y0 = frame_bottom * em;
        const double x1 = (frame_left + frame_side) * em, y1 = (frame_bottom + frame_side) * em;
        const double stroke = frame_stroke * em;
        if (!add_rectangle(contours, x0, y0, x1, y1, 1) ||
            !add_rectangle(contours, x0 + stroke, y0 + stroke, x1 - stroke, y1 - stroke, 0)) {
            font_contours_free(contours);
            font_fail(error, "allocation-failed", "Memory for the missing-glyph box could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
    }
    return EDDS_OK;
}

/** The outline in atlas pixels, its union, and the whole-pixel box the engine centres in a cell. */
static edds_status prepare_job(const font_face *face, glyph_job *job, double scale, edds_error *error) {
    font_contours contours;
    uint32_t advance = 0;
    double box[4];
    edds_status status = job_contours(face, job, &contours, &advance, error);
    if (status != EDDS_OK) return status;
    status = font_shape_of_contours(&contours, scale, &job->shape, error);
    font_contours_free(&contours);
    if (status == EDDS_OK) status = font_shape_union(&job->shape, &job->boundary, error);
    if (status != EDDS_OK) return status;
    job->advance = (int32_t)rounded(advance * scale);
    if (font_shape_bounds(&job->boundary, box)) {
        job->box_x = (int32_t)floor(box[0]);
        job->box_y = (int32_t)ceil(box[3]);
        job->width = (uint32_t)((int32_t)ceil(box[2]) - job->box_x);
        job->height = (uint32_t)(job->box_y - (int32_t)floor(box[1]));
    }
    if (job->box_x < INT16_MIN || job->box_y > INT16_MAX || job->width > 4096u || job->height > 4096u ||
        job->advance < INT16_MIN || job->advance > INT16_MAX) {
        font_fail(error, "glyph-size-limit", "A glyph is too large for an atlas.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    return EDDS_OK;
}

static void free_jobs(glyph_job *jobs, size_t count) {
    if (jobs == NULL) return;
    for (size_t at = 0; at < count; ++at) {
        font_shape_free(&jobs[at].shape);
        font_shape_free(&jobs[at].boundary);
    }
    free(jobs);
}

/** The smallest power-of-two atlas, wider than tall if not square, that holds every cell. */
static int choose_atlas(size_t cells, uint32_t cell, uint32_t *width, uint32_t *height, uint32_t *columns) {
    /* Smallest area first; within one area, the squarest shape first. 2^24 is 4096 by 4096. */
    for (uint32_t area_bits = 0; area_bits <= 24u; ++area_bits) {
        for (int height_bits = (int)(area_bits / 2u); height_bits >= 0; --height_bits) {
            const uint32_t width_bits = area_bits - (uint32_t)height_bits;
            uint32_t across, down;
            if (width_bits > 12u) break;
            /* One pixel of zeros around and between cells. */
            across = ((1u << width_bits) - 1u) / (cell + 1u);
            down = ((1u << (uint32_t)height_bits) - 1u) / (cell + 1u);
            if ((uint64_t)across * down >= cells) {
                *width = 1u << width_bits;
                *height = 1u << (uint32_t)height_bits;
                *columns = across;
                return 1;
            }
        }
    }
    return 0;
}

static void render_task(void *context, uint32_t index, edds_pool *pool) {
    render_run *run = context;
    glyph_job *job = &run->jobs[index];
    if (run->cancelled != NULL && run->cancelled(run->cancel_context)) {
        job->status = EDDS_CANCELLED;
        font_fail(&job->error, "cancelled", "The font generation was cancelled.");
    } else {
        job->status = font_field_render(&job->boundary, &job->shape, &job->placement,
            run->atlas, run->atlas_width, &job->error);
    }
    edds_pool_lock_output(pool);
    ++run->finished;
    if (run->progress != NULL) run->progress(run->progress_context, 0.1 + 0.85 * (double)run->finished / (double)run->count);
    edds_pool_unlock_output(pool);
}

static int ascending(const void *left, const void *right) {
    const uint32_t a = ((const character *)left)->code, b = ((const character *)right)->code;
    return (a > b) - (a < b);
}

static int push_code(uint32_t **list, size_t *count, uint32_t code) {
    uint32_t *grown = realloc(*list, (*count + 1u) * sizeof *grown);
    if (grown == NULL) return 0;
    grown[(*count)++] = code;
    *list = grown;
    return 1;
}

/** OS/2.sCapHeight, else the top of H (or of Cyrillic Н), rounded the way Font Editor rounds. */
static float cap_height_of(const font_face *face, double scale, const character *characters, size_t count,
    const glyph_job *jobs) {
    static const uint32_t probes[] = { 0x48u, 0x41Du };
    if (face->cap_height > 0) return (float)rounded(face->cap_height * scale);
    for (size_t probe = 0; probe < sizeof probes / sizeof probes[0]; ++probe) {
        for (size_t at = 0; at < count; ++at) {
            const glyph_job *job = &jobs[characters[at].job];
            if (characters[at].code == probes[probe] && job->origin == FROM_FONT && job->height > 0) {
                double box[4];
                if (font_shape_bounds(&job->boundary, box)) return (float)rounded(box[3]);
            }
        }
    }
    return (float)rounded(0.7 * face->units_per_em * scale);
}

edds_status font_generate(
    const font_request *request,
    font_output *output,
    edds_cancelled_fn cancelled,
    void *cancel_context,
    edds_progress_fn progress,
    void *progress_context,
    edds_error *error
) {
    font_face face;
    font_characters builtin = { NULL, 0 };
    const font_characters *wanted;
    character *characters = NULL;
    glyph_job *jobs = NULL;
    font_entry *entries = NULL;
    uint32_t *codes = NULL, *glyphs = NULL;
    font_pair *pairs = NULL;
    size_t character_count = 0, job_count = 0, pair_count = 0;
    double scale;
    uint32_t largest = 0, columns = 0;
    edds_status status;
    if (request == NULL || output == NULL || request->data == NULL || request->name == NULL) {
        font_fail(error, "invalid-api-argument", "The font request and output are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(output, 0, sizeof *output);
    if (request->font_size < FONT_MIN_SIZE || request->font_size > FONT_MAX_SIZE) {
        font_fail(error, "font-size-out-of-range", "FontSize %u is outside %u to %u atlas pixels.",
            request->font_size, FONT_MIN_SIZE, FONT_MAX_SIZE);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    status = font_face_open(&face, request->data, request->size, error);
    if (status != EDDS_OK) return status;
    font_face_names(&face, &output->source);
    output->source.units_per_em = face.units_per_em;
    output->source.glyph_count = face.glyph_count;
    if (request->characters == NULL) {
        status = font_characters_builtin(&builtin, error);
        if (status != EDDS_OK) return status;
        wanted = &builtin;
    } else {
        wanted = request->characters;
    }
    scale = (double)request->font_size / face.units_per_em;

    /* The wanted set and the two characters every font carries, each once, in ascending order. */
    characters = malloc((wanted->count + 2u) * sizeof *characters);
    jobs = calloc(wanted->count + 2u, sizeof *jobs);
    if (characters == NULL || jobs == NULL) goto out_of_memory;
    characters[character_count++].code = FONT_SPACE;
    characters[character_count++].code = FONT_MISSING_BOX;
    for (size_t at = 0; at < wanted->count; ++at) {
        if (wanted->codes[at] != FONT_SPACE && wanted->codes[at] != FONT_MISSING_BOX) {
            characters[character_count++].code = wanted->codes[at];
        }
    }
    qsort(characters, character_count, sizeof *characters, ascending);
    {
        size_t kept = 0;
        for (size_t at = 0; at < character_count; ++at) {
            const uint32_t glyph = font_face_glyph(&face, characters[at].code);
            const int mandatory = characters[at].code == FONT_SPACE || characters[at].code == FONT_MISSING_BOX;
            size_t job = job_count;
            if (glyph == 0 && !mandatory) {
                if (!push_code(&output->missing, &output->missing_count, characters[at].code)) goto out_of_memory;
                continue;
            }
            if (glyph == 0 && !push_code(&output->drawn, &output->drawn_count, characters[at].code)) goto out_of_memory;
            /* Characters that share a glyph share its cell. */
            for (size_t seen = 0; glyph != 0 && seen < job_count; ++seen) {
                if (jobs[seen].origin == FROM_FONT && jobs[seen].glyph == glyph) job = seen;
            }
            if (job == job_count) {
                jobs[job].origin = glyph != 0 ? FROM_FONT : (characters[at].code == FONT_SPACE ? DRAWN_SPACE : DRAWN_BOX);
                jobs[job].glyph = glyph;
                ++job_count;
            }
            characters[kept].code = characters[at].code;
            characters[kept].glyph = glyph;
            characters[kept].job = job;
            ++kept;
        }
        character_count = kept;
    }
    if (progress != NULL) progress(progress_context, 0.02);

    for (size_t at = 0; at < job_count; ++at) {
        if (cancelled != NULL && cancelled(cancel_context)) {
            font_fail(error, "cancelled", "The font generation was cancelled.");
            status = EDDS_CANCELLED;
            goto done;
        }
        status = prepare_job(&face, &jobs[at], scale, error);
        if (status != EDDS_OK) goto done;
        if (jobs[at].width > largest) largest = jobs[at].width;
        if (jobs[at].height > largest) largest = jobs[at].height;
    }
    if (progress != NULL) progress(progress_context, 0.1);

    output->cell = largest + CELL_MARGIN;
    if (!choose_atlas(job_count, output->cell, &output->atlas_width, &output->atlas_height, &columns)) {
        font_fail(error, "atlas-too-large", "%zu glyphs in %u-pixel cells do not fit one %u by %u atlas.",
            job_count, output->cell, FONT_MAX_ATLAS, FONT_MAX_ATLAS);
        status = EDDS_UNSUPPORTED_FORMAT;
        goto done;
    }
    for (size_t at = 0; at < job_count; ++at) {
        font_placement *placement = &jobs[at].placement;
        placement->cell = output->cell;
        placement->cell_x = 1u + (uint32_t)(at % columns) * (output->cell + 1u);
        placement->cell_y = 1u + (uint32_t)(at / columns) * (output->cell + 1u);
        placement->box_x = jobs[at].box_x;
        placement->box_y = jobs[at].box_y;
        placement->width = jobs[at].width;
        placement->height = jobs[at].height;
    }
    output->atlas = calloc((size_t)output->atlas_width * output->atlas_height, 4u);
    if (output->atlas == NULL) goto out_of_memory;
    for (size_t at = 3; at < (size_t)output->atlas_width * output->atlas_height * 4u; at += 4u) output->atlas[at] = 255u;
    {
        render_run run;
        run.jobs = jobs;
        run.count = job_count;
        run.atlas = output->atlas;
        run.atlas_width = output->atlas_width;
        run.cancelled = cancelled;
        run.cancel_context = cancel_context;
        run.progress = progress;
        run.progress_context = progress_context;
        run.finished = 0;
        status = edds_pool_run((uint32_t)job_count, EDDS_POOL_MEMORY_BUDGET, render_task, &run, error);
        if (status != EDDS_OK) goto done;
        for (size_t at = 0; at < job_count; ++at) {
            if (jobs[at].status != EDDS_OK) {
                status = jobs[at].status;
                *error = jobs[at].error;
                goto done;
            }
        }
    }

    codes = malloc(character_count * sizeof *codes);
    glyphs = malloc(character_count * sizeof *glyphs);
    entries = malloc(character_count * sizeof *entries);
    if (codes == NULL || glyphs == NULL || entries == NULL) goto out_of_memory;
    for (size_t at = 0; at < character_count; ++at) {
        const glyph_job *job = &jobs[characters[at].job];
        codes[at] = characters[at].code;
        glyphs[at] = characters[at].glyph;
        entries[at].code = characters[at].code;
        entries[at].x = (uint16_t)job->placement.cell_x;
        entries[at].y = (uint16_t)job->placement.cell_y;
        entries[at].width = (uint16_t)job->width;
        entries[at].height = (uint16_t)job->height;
        entries[at].box_x = (int16_t)job->box_x;
        entries[at].box_y = (int16_t)job->box_y;
        entries[at].advance = (int16_t)job->advance;
    }
    status = font_face_kerning(&face, codes, glyphs, character_count, scale, &pairs, &pair_count, error);
    if (status != EDDS_OK) goto done;
    {
        font_header header;
        header.name = request->name;
        header.size = request->font_size;
        header.cell = output->cell;
        header.cap_height = cap_height_of(&face, scale, characters, character_count, jobs);
        status = font_fnt_write(&header, entries, character_count, pairs, pair_count,
            &output->fnt, &output->fnt_size, &output->range_count, error);
    }
    output->glyph_count = (uint32_t)character_count;
    output->pair_count = (uint32_t)pair_count;
    if (progress != NULL && status == EDDS_OK) progress(progress_context, 1.0);
    goto done;

out_of_memory:
    font_fail(error, "allocation-failed", "Memory for the font could not be allocated.");
    status = EDDS_INTERNAL_FAILURE;

done:
    free_jobs(jobs, job_count);
    free(characters);
    free(entries);
    free(codes);
    free(glyphs);
    free(pairs);
    font_characters_free(&builtin);
    if (status != EDDS_OK) font_output_free(output);
    return status;
}

void font_output_free(font_output *output) {
    if (output == NULL) return;
    free(output->fnt);
    free(output->atlas);
    free(output->missing);
    free(output->drawn);
    memset(output, 0, sizeof *output);
}

edds_status font_source_describe(const uint8_t *data, size_t size, font_source_info *info, edds_error *error) {
    font_face face;
    edds_status status;
    if (info == NULL) {
        font_fail(error, "invalid-api-argument", "The font description is required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(info, 0, sizeof *info);
    status = font_face_open(&face, data, size, error);
    if (status != EDDS_OK) return status;
    font_face_names(&face, info);
    info->units_per_em = face.units_per_em;
    info->glyph_count = face.glyph_count;
    return EDDS_OK;
}
