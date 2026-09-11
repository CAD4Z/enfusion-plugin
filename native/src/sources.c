/*
 * The one table of source resource classes this converter supports. Adding a format is adding a
 * row here and a decoder behind it; nothing else in the converter carries its own list.
 */
#include <edds/edds.h>

#include <string.h>

static const edds_source_capability capabilities[] = {
    { EDDS_SOURCE_PNG, ".png", "png", "PNGResourceClass" },
    { EDDS_SOURCE_TGA, ".tga", "tga", "TGAResourceClass" },
    { EDDS_SOURCE_JPG, ".jpg", "jpg", "JPGResourceClass" },
    { EDDS_SOURCE_TIFF, ".tiff", "tiff", "TIFFResourceClass" }
};

const edds_source_capability *edds_source_capabilities(size_t *count) {
    if (count != NULL) {
        *count = sizeof capabilities / sizeof capabilities[0];
    }
    return capabilities;
}

const edds_source_capability *edds_source_capability_of_format(edds_source_format format) {
    for (size_t at = 0; at < sizeof capabilities / sizeof capabilities[0]; ++at) {
        if (capabilities[at].format == format) {
            return &capabilities[at];
        }
    }
    return NULL;
}

const edds_source_capability *edds_source_capability_of_resource_class(const char *resource_class) {
    if (resource_class == NULL) {
        return NULL;
    }
    for (size_t at = 0; at < sizeof capabilities / sizeof capabilities[0]; ++at) {
        if (strcmp(capabilities[at].resource_class, resource_class) == 0) {
            return &capabilities[at];
        }
    }
    return NULL;
}
