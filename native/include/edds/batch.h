#ifndef EDDS_BATCH_H
#define EDDS_BATCH_H

#include <edds/edds.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDDS_BATCH_MAX_JOBS       256u
#define EDDS_BATCH_MAX_LINE_BYTES ((size_t)256 * 1024)
#define EDDS_BATCH_ID_BYTES       128u
#define EDDS_BATCH_PATH_BYTES     32768u

typedef enum edds_batch_record_kind {
    EDDS_BATCH_HEADER,
    EDDS_BATCH_JOB,
    EDDS_BATCH_END
} edds_batch_record_kind;

typedef struct edds_batch_job {
    char         id[EDDS_BATCH_ID_BYTES];
    char         input[EDDS_BATCH_PATH_BYTES];
    char         output[EDDS_BATCH_PATH_BYTES];
    int          has_metadata;
    char         metadata[EDDS_BATCH_PATH_BYTES];
    char         resource_name[EDDS_METADATA_PATH_BYTES];
    char         source_file[EDDS_METADATA_PATH_BYTES];
    char         guid[EDDS_METADATA_GUID_BYTES];
    edds_profile profile;
    int          has_expected;
    char         expected_source[64];
    char         expected_output[64];
    char         expected_metadata[64];
} edds_batch_job;

typedef struct edds_batch_record {
    edds_batch_record_kind kind;
    uint32_t               job_count;
    edds_batch_job         job;
} edds_batch_record;

/** Parses one complete UTF-8 NDJSON record and rejects unknown/duplicate/incomplete fields. */
edds_status edds_batch_parse_line(
    const char        *line,
    size_t             size,
    edds_batch_record *record,
    edds_error        *error);

/**
 * Frames NDJSON lines out of a byte stream. Where a read happened to split is not something the
 * protocol says anything about, so the reader carries a partial line between pushes and only ever
 * hands back whole ones. A line over the hard limit is refused once, and the rest of that line is
 * swallowed rather than framed into a record that was never sent.
 */
typedef struct edds_batch_reader {
    char   line[EDDS_BATCH_MAX_LINE_BYTES + 1u];
    size_t size;
    int    overflowed;
} edds_batch_reader;

typedef enum edds_batch_line {
    EDDS_BATCH_LINE_PENDING,
    EDDS_BATCH_LINE_READY,
    EDDS_BATCH_LINE_OVERFLOW
} edds_batch_line;

void edds_batch_reader_init(edds_batch_reader *reader);

/**
 * Takes bytes and reports the first line they completed. `consumed` says how much of `data` was
 * taken, so the caller pushes the rest back in to collect the lines behind it. A READY result
 * leaves the line NUL-terminated in `reader->line` with its length in `*size`.
 */
edds_batch_line edds_batch_reader_push(
    edds_batch_reader *reader,
    const char        *data,
    size_t             data_size,
    size_t            *consumed,
    size_t            *size);

/** The last line of a stream that ended without a newline; PENDING when nothing was left. */
edds_batch_line edds_batch_reader_finish(edds_batch_reader *reader, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
