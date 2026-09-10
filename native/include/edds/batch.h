#ifndef EDDS_BATCH_H
#define EDDS_BATCH_H

#include <edds/edds.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDDS_BATCH_MAX_JOBS 256u
#define EDDS_BATCH_MAX_LINE_BYTES ((size_t)256 * 1024)
#define EDDS_BATCH_ID_BYTES 128u
#define EDDS_BATCH_PATH_BYTES 32768u

typedef enum edds_batch_record_kind {
    EDDS_BATCH_HEADER,
    EDDS_BATCH_JOB,
    EDDS_BATCH_END
} edds_batch_record_kind;

typedef struct edds_batch_job {
    char id[EDDS_BATCH_ID_BYTES];
    char input[EDDS_BATCH_PATH_BYTES];
    char output[EDDS_BATCH_PATH_BYTES];
    int has_metadata;
    char metadata[EDDS_BATCH_PATH_BYTES];
    char resource_name[EDDS_METADATA_PATH_BYTES];
    char source_file[EDDS_METADATA_PATH_BYTES];
    char guid[EDDS_METADATA_GUID_BYTES];
    edds_profile profile;
    int has_expected;
    char expected_source[64];
    char expected_output[64];
    char expected_metadata[64];
} edds_batch_job;

typedef struct edds_batch_record {
    edds_batch_record_kind kind;
    uint32_t job_count;
    edds_batch_job job;
} edds_batch_record;

/** Parses one complete UTF-8 NDJSON record and rejects unknown/duplicate/incomplete fields. */
edds_status edds_batch_parse_line(
    const char *line,
    size_t size,
    edds_batch_record *record,
    edds_error *error
);

#ifdef __cplusplus
}
#endif

#endif
