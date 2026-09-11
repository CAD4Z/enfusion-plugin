#include <edds/edds.h>
#include <edds/batch.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FILE *file = tmpfile();
    FILE *output = tmpfile();
    edds_info info;
    edds_metadata metadata;
    edds_profile profile;
    edds_error error;
    edds_batch_record batch;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    if (file == NULL || output == NULL) {
        if (file != NULL) fclose(file);
        if (output != NULL) fclose(output);
        return 0;
    }
    if (size != 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        fclose(output);
        return 0;
    }
    rewind(file);
    if (edds_inspect(file, &info, NULL, NULL, &error) == EDDS_OK && info.mip_count != 0) {
        (void)edds_preview(file, &info, 0, NULL, NULL, &rgba, &rgba_size, &error);
        edds_free(rgba);
    }
    /*
     * The same bytes as every source format in the contract, so a corpus entry that is a truncated
     * PNG is also a malformed JPEG and an oversized TIFF directory to the decoder beside it.
     */
    edds_default_profile(&profile);
    {
        size_t formats = 0;
        const edds_source_capability *capabilities = edds_source_capabilities(&formats);
        for (size_t at = 0; at < formats; ++at) {
            rewind(file);
            rewind(output);
            (void)edds_convert(file, capabilities[at].format, output, &profile,
                NULL, NULL, NULL, NULL, &error);
        }
    }
    rewind(file);
    (void)edds_metadata_parse(file, &metadata, &error);
    (void)edds_batch_parse_line((const char *)data, size, &batch, &error);
    /*
     * The same bytes again as a stream, split where the input itself says to split them, so the
     * framing boundary sees fragments, oversized lines and records straddling a chunk edge.
     */
    {
        edds_batch_reader *reader = malloc(sizeof *reader);
        if (reader != NULL) {
            size_t at = 0;
            edds_batch_reader_init(reader);
            while (at < size) {
                const size_t step = (size_t)(data[at] == 0u ? 1u : data[at]);
                size_t remaining = size - at < step ? size - at : step;
                const char *chunk = (const char *)data + at;
                at += remaining;
                while (remaining > 0u) {
                    size_t consumed = 0;
                    size_t line_size = 0;
                    const edds_batch_line line =
                        edds_batch_reader_push(reader, chunk, remaining, &consumed, &line_size);
                    if (consumed == 0u) break;
                    chunk += consumed;
                    remaining -= consumed;
                    if (line == EDDS_BATCH_LINE_READY) {
                        (void)edds_batch_parse_line(reader->line, line_size, &batch, &error);
                    }
                }
            }
            {
                size_t line_size = 0;
                if (edds_batch_reader_finish(reader, &line_size) == EDDS_BATCH_LINE_READY) {
                    (void)edds_batch_parse_line(reader->line, line_size, &batch, &error);
                }
            }
            free(reader);
        }
    }
    fclose(file);
    fclose(output);
    return 0;
}
