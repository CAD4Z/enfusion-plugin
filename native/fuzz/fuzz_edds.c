#include <edds/edds.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FILE *file = tmpfile();
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    if (file == NULL) {
        return 0;
    }
    if (size != 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        return 0;
    }
    rewind(file);
    if (edds_inspect(file, &info, NULL, NULL, &error) == EDDS_OK && info.mip_count != 0) {
        (void)edds_preview(file, &info, 0, NULL, NULL, &rgba, &rgba_size, &error);
        edds_free(rgba);
    }
    fclose(file);
    return 0;
}
