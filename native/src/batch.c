#include <edds/batch.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct json_scan {
    const char *at;
    const char *end;
} json_scan;

static edds_status fail(edds_error *error, const char *code, const char *format, ...) {
    va_list arguments;
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof error->message, format, arguments);
    va_end(arguments);
    return EDDS_INVALID_INVOCATION;
}

static void space(json_scan *scan) {
    while (scan->at < scan->end && (*scan->at == ' ' || *scan->at == '\t' ||
            *scan->at == '\r' || *scan->at == '\n')) ++scan->at;
}

static int take(json_scan *scan, char value) {
    space(scan);
    if (scan->at == scan->end || *scan->at != value) return 0;
    ++scan->at;
    return 1;
}

static int literal(json_scan *scan, const char *value) {
    const size_t size = strlen(value);
    space(scan);
    if ((size_t)(scan->end - scan->at) < size || memcmp(scan->at, value, size) != 0) return 0;
    scan->at += size;
    return 1;
}

static int hex(char value, uint32_t *digit) {
    if (value >= '0' && value <= '9') *digit = (uint32_t)(value - '0');
    else if (value >= 'a' && value <= 'f') *digit = (uint32_t)(value - 'a') + 10u;
    else if (value >= 'A' && value <= 'F') *digit = (uint32_t)(value - 'A') + 10u;
    else return 0;
    return 1;
}

static int append_byte(char *output, size_t capacity, size_t *size, uint8_t value) {
    if (*size + 1u >= capacity) return 0;
    output[(*size)++] = (char)value;
    return 1;
}

static int append_codepoint(char *output, size_t capacity, size_t *size, uint32_t value) {
    if (value <= 0x7fu) return append_byte(output, capacity, size, (uint8_t)value);
    if (value <= 0x7ffu) {
        return append_byte(output, capacity, size, (uint8_t)(0xc0u | (value >> 6u))) &&
            append_byte(output, capacity, size, (uint8_t)(0x80u | (value & 0x3fu)));
    }
    if (value >= 0xd800u && value <= 0xdfffu) return 0;
    return append_byte(output, capacity, size, (uint8_t)(0xe0u | (value >> 12u))) &&
        append_byte(output, capacity, size, (uint8_t)(0x80u | ((value >> 6u) & 0x3fu))) &&
        append_byte(output, capacity, size, (uint8_t)(0x80u | (value & 0x3fu)));
}

static int string_value(json_scan *scan, char *output, size_t capacity) {
    size_t size = 0;
    if (!take(scan, '"') || capacity == 0u) return 0;
    while (scan->at < scan->end && *scan->at != '"') {
        unsigned char value = (unsigned char)*scan->at++;
        if (value < 0x20u) return 0;
        if (value == '\\') {
            uint32_t codepoint = 0;
            if (scan->at == scan->end) return 0;
            value = (unsigned char)*scan->at++;
            switch (value) {
                case '"': case '\\': case '/': break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'u':
                    if (scan->end - scan->at < 4) return 0;
                    for (unsigned at = 0; at < 4u; ++at) {
                        uint32_t digit;
                        if (!hex(*scan->at++, &digit)) return 0;
                        codepoint = codepoint * 16u + digit;
                    }
                    if (!append_codepoint(output, capacity, &size, codepoint)) return 0;
                    continue;
                default: return 0;
            }
        }
        if (!append_byte(output, capacity, &size, value)) return 0;
    }
    if (scan->at == scan->end) return 0;
    ++scan->at;
    output[size] = '\0';
    return 1;
}

static int unsigned_value(json_scan *scan, uint32_t *result) {
    uint64_t value = 0;
    int digits = 0;
    space(scan);
    while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') {
        value = value * 10u + (unsigned)(*scan->at++ - '0');
        if (value > UINT32_MAX) return 0;
        digits = 1;
    }
    if (!digits) return 0;
    *result = (uint32_t)value;
    return 1;
}

static int skip_value(json_scan *scan, unsigned depth);

static int skip_string(json_scan *scan) {
    char scratch[2];
    const char *start = scan->at;
    if (string_value(scan, scratch, sizeof scratch)) return 1;
    scan->at = start;
    if (!take(scan, '"')) return 0;
    while (scan->at < scan->end) {
        unsigned char value = (unsigned char)*scan->at++;
        if (value == '"') return 1;
        if (value < 0x20u) return 0;
        if (value == '\\') {
            if (scan->at == scan->end) return 0;
            value = (unsigned char)*scan->at++;
            if (value == 'u') {
                if (scan->end - scan->at < 4) return 0;
                for (unsigned at = 0; at < 4u; ++at) {
                    uint32_t digit;
                    if (!hex(*scan->at++, &digit)) return 0;
                }
            } else if (strchr("\"\\/bfnrt", value) == NULL) return 0;
        }
    }
    return 0;
}

static int skip_value(json_scan *scan, unsigned depth) {
    if (depth > 32u) return 0;
    space(scan);
    if (scan->at == scan->end) return 0;
    if (*scan->at == '"') return skip_string(scan);
    if (*scan->at == '{') {
        ++scan->at;
        space(scan);
        if (take(scan, '}')) return 1;
        for (;;) {
            if (!skip_string(scan) || !take(scan, ':') || !skip_value(scan, depth + 1u)) return 0;
            if (take(scan, '}')) return 1;
            if (!take(scan, ',')) return 0;
        }
    }
    if (*scan->at == '[') {
        ++scan->at;
        space(scan);
        if (take(scan, ']')) return 1;
        for (;;) {
            if (!skip_value(scan, depth + 1u)) return 0;
            if (take(scan, ']')) return 1;
            if (!take(scan, ',')) return 0;
        }
    }
    if (literal(scan, "true") || literal(scan, "false") || literal(scan, "null")) return 1;
    {
        const char *start = scan->at;
        if (*scan->at == '-') ++scan->at;
        while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') ++scan->at;
        if (scan->at < scan->end && *scan->at == '.') {
            ++scan->at;
            while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') ++scan->at;
        }
        if (scan->at < scan->end && (*scan->at == 'e' || *scan->at == 'E')) {
            ++scan->at;
            if (scan->at < scan->end && (*scan->at == '+' || *scan->at == '-')) ++scan->at;
            while (scan->at < scan->end && *scan->at >= '0' && *scan->at <= '9') ++scan->at;
        }
        return scan->at > start;
    }
}

static int discover(const char *line, size_t size, uint32_t *version, char kind[32]) {
    json_scan scan = { line, line + size };
    int have_version = 0;
    int have_kind = 0;
    if (!take(&scan, '{')) return 0;
    if (take(&scan, '}')) return 0;
    for (;;) {
        char key[64];
        if (!string_value(&scan, key, sizeof key) || !take(&scan, ':')) return 0;
        if (strcmp(key, "protocolVersion") == 0) {
            if (have_version || !unsigned_value(&scan, version)) return 0;
            have_version = 1;
        } else if (strcmp(key, "kind") == 0) {
            if (have_kind || !string_value(&scan, kind, 32u)) return 0;
            have_kind = 1;
        } else if (!skip_value(&scan, 0u)) return 0;
        if (take(&scan, '}')) break;
        if (!take(&scan, ',')) return 0;
    }
    space(&scan);
    return scan.at == scan.end && have_version && have_kind;
}

static int bool_value(json_scan *scan, int *result) {
    if (literal(scan, "true")) { *result = 1; return 1; }
    if (literal(scan, "false")) { *result = 0; return 1; }
    return 0;
}

static int profile_value(json_scan *scan, edds_profile *profile) {
    unsigned fields = 0;
    if (!take(scan, '{')) return 0;
    edds_default_profile(profile);
    for (;;) {
        char key[64];
        char value[64];
        uint32_t number;
        int boolean;
        unsigned bit;
        if (!string_value(scan, key, sizeof key) || !take(scan, ':')) return 0;
        if (strcmp(key, "TargetFormat") == 0) {
            bit = 1u << 0; if (!string_value(scan, value, sizeof value) || strcmp(value, "EnfusionDDS") != 0) return 0;
        } else if (strcmp(key, "FormatCompress") == 0) {
            bit = 1u << 1; if (!string_value(scan, value, sizeof value)) return 0;
            if (strcmp(value, "Copy") == 0) profile->format_compress = EDDS_COMPRESS_COPY;
            else if (strcmp(value, "Fastest") == 0) profile->format_compress = EDDS_COMPRESS_FASTEST;
            else if (strcmp(value, "Medium") == 0) profile->format_compress = EDDS_COMPRESS_MEDIUM;
            else if (strcmp(value, "Best") == 0) profile->format_compress = EDDS_COMPRESS_BEST;
            else return 0;
        } else if (strcmp(key, "CompressTreshold") == 0) {
            bit = 1u << 2; if (!unsigned_value(scan, &number) || number > 100u) return 0; profile->compress_threshold = number;
        } else if (strcmp(key, "Conversion") == 0) {
            bit = 1u << 3; if (!string_value(scan, value, sizeof value) || strcmp(value, "None") != 0) return 0;
        } else if (strcmp(key, "ConversionQuality") == 0) {
            bit = 1u << 4; if (!unsigned_value(scan, &number) || number != 1u) return 0;
        } else if (strcmp(key, "Swizzling") == 0) {
            bit = 1u << 5; if (!string_value(scan, value, sizeof value) || strcmp(value, "None") != 0) return 0;
        } else if (strcmp(key, "GenerateMips") == 0) {
            bit = 1u << 6; if (!bool_value(scan, &boolean)) return 0; profile->generate_mips = boolean;
        } else if (strcmp(key, "MipMapFunction") == 0) {
            bit = 1u << 7; if (!string_value(scan, value, sizeof value) || strcmp(value, "Filter") != 0) return 0;
        } else if (strcmp(key, "MipMapFilter") == 0) {
            bit = 1u << 8; if (!string_value(scan, value, sizeof value) || strcmp(value, "Box") != 0) return 0;
        } else if (strcmp(key, "TiledTexture") == 0) {
            bit = 1u << 9; if (!bool_value(scan, &boolean) || !boolean) return 0;
        } else return 0;
        if ((fields & bit) != 0u) return 0;
        fields |= bit;
        if (take(scan, '}')) break;
        if (!take(scan, ',')) return 0;
    }
    return fields == 0x3ffu;
}

static int identity_value(json_scan *scan, edds_batch_job *job) {
    unsigned fields = 0;
    if (literal(scan, "null")) return 1;
    if (!take(scan, '{')) return 0;
    for (;;) {
        char key[64];
        unsigned bit;
        if (!string_value(scan, key, sizeof key) || !take(scan, ':')) return 0;
        if (strcmp(key, "guid") == 0) {
            bit = 1u; if (!string_value(scan, job->guid, sizeof job->guid)) return 0;
        } else if (strcmp(key, "name") == 0) {
            bit = 2u; if (!string_value(scan, job->resource_name, sizeof job->resource_name)) return 0;
        } else if (strcmp(key, "sourceFile") == 0) {
            bit = 4u; if (!string_value(scan, job->source_file, sizeof job->source_file)) return 0;
        } else return 0;
        if ((fields & bit) != 0u) return 0;
        fields |= bit;
        if (take(scan, '}')) break;
        if (!take(scan, ',')) return 0;
    }
    return fields == 7u;
}

static int expected_value(json_scan *scan, edds_batch_job *job) {
    unsigned fields = 0;
    if (literal(scan, "null")) return 1;
    if (!take(scan, '{')) return 0;
    job->has_expected = 1;
    for (;;) {
        char key[64];
        char *output;
        unsigned bit;
        if (!string_value(scan, key, sizeof key) || !take(scan, ':')) return 0;
        if (strcmp(key, "source") == 0) { bit = 1u; output = job->expected_source; }
        else if (strcmp(key, "output") == 0) { bit = 2u; output = job->expected_output; }
        else if (strcmp(key, "metadata") == 0) { bit = 4u; output = job->expected_metadata; }
        else return 0;
        if ((fields & bit) != 0u || !string_value(scan, output, 64u)) return 0;
        fields |= bit;
        if (take(scan, '}')) break;
        if (!take(scan, ',')) return 0;
    }
    return fields == 7u;
}

static int simple_record(const char *line, size_t size, const char *expected_kind, uint32_t *count) {
    json_scan scan = { line, line + size };
    unsigned fields = 0;
    if (!take(&scan, '{')) return 0;
    for (;;) {
        char key[64];
        char value[32];
        uint32_t number;
        unsigned bit;
        if (!string_value(&scan, key, sizeof key) || !take(&scan, ':')) return 0;
        if (strcmp(key, "protocolVersion") == 0) {
            bit = 1u; if (!unsigned_value(&scan, &number) || number != 1u) return 0;
        } else if (strcmp(key, "kind") == 0) {
            bit = 2u; if (!string_value(&scan, value, sizeof value) || strcmp(value, expected_kind) != 0) return 0;
        } else if (count != NULL && strcmp(key, "jobCount") == 0) {
            bit = 4u; if (!unsigned_value(&scan, count)) return 0;
        } else return 0;
        if ((fields & bit) != 0u) return 0;
        fields |= bit;
        if (take(&scan, '}')) break;
        if (!take(&scan, ',')) return 0;
    }
    space(&scan);
    return scan.at == scan.end && fields == (count == NULL ? 3u : 7u);
}

static int job_record(const char *line, size_t size, edds_batch_job *job) {
    json_scan scan = { line, line + size };
    unsigned fields = 0;
    memset(job, 0, sizeof *job);
    if (!take(&scan, '{')) return 0;
    for (;;) {
        char key[64];
        char value[32];
        uint32_t number;
        unsigned bit;
        if (!string_value(&scan, key, sizeof key) || !take(&scan, ':')) return 0;
        if (strcmp(key, "protocolVersion") == 0) {
            bit = 1u << 0; if (!unsigned_value(&scan, &number) || number != 1u) return 0;
        } else if (strcmp(key, "kind") == 0) {
            bit = 1u << 1; if (!string_value(&scan, value, sizeof value) || strcmp(value, "job") != 0) return 0;
        } else if (strcmp(key, "id") == 0) {
            bit = 1u << 2; if (!string_value(&scan, job->id, sizeof job->id) || job->id[0] == '\0') return 0;
        } else if (strcmp(key, "input") == 0) {
            bit = 1u << 3; if (!string_value(&scan, job->input, sizeof job->input) || job->input[0] == '\0') return 0;
        } else if (strcmp(key, "output") == 0) {
            bit = 1u << 4; if (!string_value(&scan, job->output, sizeof job->output) || job->output[0] == '\0') return 0;
        } else if (strcmp(key, "metadata") == 0) {
            bit = 1u << 5;
            if (literal(&scan, "null")) job->has_metadata = 0;
            else { job->has_metadata = 1; if (!string_value(&scan, job->metadata, sizeof job->metadata)) return 0; }
        } else if (strcmp(key, "identity") == 0) {
            bit = 1u << 6; if (!identity_value(&scan, job)) return 0;
        } else if (strcmp(key, "profile") == 0) {
            bit = 1u << 7; if (!profile_value(&scan, &job->profile)) return 0;
        } else if (strcmp(key, "expected") == 0) {
            bit = 1u << 8; if (!expected_value(&scan, job)) return 0;
        } else return 0;
        if ((fields & bit) != 0u) return 0;
        fields |= bit;
        if (take(&scan, '}')) break;
        if (!take(&scan, ',')) return 0;
    }
    space(&scan);
    if (scan.at != scan.end || fields != 0x1ffu) return 0;
    return job->has_metadata
        ? job->metadata[0] != '\0' && job->guid[0] != '\0' &&
            job->resource_name[0] != '\0' && job->source_file[0] != '\0'
        : job->guid[0] == '\0' && job->resource_name[0] == '\0' && job->source_file[0] == '\0';
}

edds_status edds_batch_parse_line(
    const char *line,
    size_t size,
    edds_batch_record *record,
    edds_error *error
) {
    uint32_t version = 0;
    char kind[32] = { 0 };
    if (line == NULL || record == NULL || error == NULL || size == 0u ||
        size > EDDS_BATCH_MAX_LINE_BYTES) {
        return fail(error, "batch-line-size", "A batch NDJSON record is empty or exceeds the hard line limit.");
    }
    if (!discover(line, size, &version, kind)) {
        return fail(error, "malformed-batch-record", "A batch input line is not a valid NDJSON object.");
    }
    if (version != 1u) {
        return fail(error, "incompatible-batch-version", "A batch input record uses an incompatible protocol version.");
    }
    memset(record, 0, sizeof *record);
    if (strcmp(kind, "batch") == 0) {
        record->kind = EDDS_BATCH_HEADER;
        if (!simple_record(line, size, "batch", &record->job_count)) goto malformed;
        return EDDS_OK;
    }
    if (strcmp(kind, "job") == 0) {
        record->kind = EDDS_BATCH_JOB;
        if (!job_record(line, size, &record->job)) goto malformed;
        return EDDS_OK;
    }
    if (strcmp(kind, "end") == 0) {
        record->kind = EDDS_BATCH_END;
        if (!simple_record(line, size, "end", NULL)) goto malformed;
        return EDDS_OK;
    }
    return fail(error, "unknown-batch-record", "A batch input record has an unknown kind.");

malformed:
    return fail(error, "malformed-batch-record", "A batch input record is incomplete, duplicated, or unsupported.");
}
