#include <edds/edds.h>
#include <edds/batch.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
    edds_default_profile(&profile);
    rewind(file);
    (void)edds_convert(file, EDDS_SOURCE_PNG, output, &profile, NULL, NULL, &error);
    rewind(file);
    rewind(output);
    (void)edds_convert(file, EDDS_SOURCE_TGA, output, &profile, NULL, NULL, &error);
    rewind(file);
    (void)edds_metadata_parse(file, &metadata, &error);
    (void)edds_batch_parse_line((const char *)data, size, &batch, &error);
    fclose(file);
    fclose(output);
    return 0;
}
