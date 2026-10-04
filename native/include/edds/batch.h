#ifndef EDDS_BATCH_H
#define EDDS_BATCH_H

#include <edds/edds.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The limits of the batch protocol: the most jobs one batch may hold, the longest line, and the
 * buffers a job's id and paths are read into, their terminating NUL included.
 */
#define EDDS_BATCH_MAX_JOBS       256u
#define EDDS_BATCH_MAX_LINE_BYTES ((size_t)256 * 1024)
#define EDDS_BATCH_ID_BYTES       128u
#define EDDS_BATCH_PATH_BYTES     32768u

/** What a line of the batch holds: the header (kind "batch"), a job, or the end. */
typedef enum edds_batch_record_kind {
    EDDS_BATCH_HEADER,
    EDDS_BATCH_JOB,
    EDDS_BATCH_END
} edds_batch_record_kind;

/** One job record: an image to convert, where its output goes, and the settings it takes. */
typedef struct edds_batch_job {
    /** The job's id, the source image, and the EDDS file it becomes. */
    char id[EDDS_BATCH_ID_BYTES];
    char input[EDDS_BATCH_PATH_BYTES];
    char output[EDDS_BATCH_PATH_BYTES];

    /** The metadata file to write, when there is one, and the "identity" it records. */
    int  has_metadata;
    char metadata[EDDS_BATCH_PATH_BYTES];
    char resource_name[EDDS_METADATA_PATH_BYTES];
    char source_file[EDDS_METADATA_PATH_BYTES];
    char guid[EDDS_METADATA_GUID_BYTES];

    /** The Workbench settings of the conversion. */
    edds_profile profile;

    /** The "expected" object, when present: the source, output and metadata revisions. */
    int  has_expected;
    char expected_source[64];
    char expected_output[64];
    char expected_metadata[64];
} edds_batch_job;

/** One line of the batch, read: its kind, and what a line of that kind carries. */
typedef struct edds_batch_record {
    edds_batch_record_kind kind;

    /** The header's "jobCount". */
    uint32_t job_count;

    /** A job record's job. */
    edds_batch_job job;
} edds_batch_record;

/**
 * Parses one complete UTF-8 NDJSON record and rejects unknown/duplicate/incomplete fields.
 * Returns EDDS_OK with `*record` filled in; EDDS_INVALID_INVOCATION when the line is not a valid
 * record; or, for a job whose profile edds_profile_check refuses, that refusal.
 */
edds_status edds_batch_parse_line(const char *line, size_t size, edds_batch_record *record, edds_error *error);

/**
 * Frames NDJSON lines out of a byte stream. Where a read happened to split is not something the
 * protocol says anything about, so the reader carries a partial line between pushes and only ever
 * hands back whole ones. A line over the hard limit is refused once, and the rest of that line is
 * swallowed rather than framed into a record that was never sent.
 */
typedef struct edds_batch_reader {
    /** The line gathered so far, and how many bytes of it there are. */
    char   line[EDDS_BATCH_MAX_LINE_BYTES + 1u];
    size_t size;

    /** Set while the rest of a line over the limit is being swallowed. */
    int overflowed;
} edds_batch_reader;

/** What a push or a finish reports: no whole line yet, a line ready, or a line over the limit. */
typedef enum edds_batch_line {
    EDDS_BATCH_LINE_PENDING,
    EDDS_BATCH_LINE_READY,
    EDDS_BATCH_LINE_OVERFLOW
} edds_batch_line;

/** Empties a reader before its first push. */
void edds_batch_reader_init(edds_batch_reader *reader);

/**
 * Takes bytes and reports the first line they completed. `consumed` says how much of `data` was
 * taken, so the caller pushes the rest back in to collect the lines behind it. A READY result
 * leaves the line NUL-terminated in `reader->line` with its length in `*size`. OVERFLOW reports
 * the end of a line over the limit; PENDING, that every byte was taken and no line is complete.
 */
edds_batch_line edds_batch_reader_push(
    edds_batch_reader *reader,
    const char        *data,
    size_t             data_size,
    size_t            *consumed,
    size_t            *size);

/**
 * The last line of a stream that ended without a newline; PENDING when nothing was left, and
 * OVERFLOW when it was a line over the limit.
 */
edds_batch_line edds_batch_reader_finish(edds_batch_reader *reader, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
