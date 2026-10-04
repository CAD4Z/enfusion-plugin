/*
 * The one table of source resource classes this converter supports. Adding a format is adding a
 * row here and a decoder behind it; nothing else in the converter carries its own list.
 */
#include <edds/edds.h>

#include <string.h>

/** One row per format: the format, its extension, wire name and Workbench resource class. */
static const edds_source_capability capabilities[] = {
    { EDDS_SOURCE_PNG, ".png", "png", "PNGResourceClass" },
    { EDDS_SOURCE_TGA, ".tga", "tga", "TGAResourceClass" },
    { EDDS_SOURCE_JPG, ".jpg", "jpg", "JPGResourceClass" },
    { EDDS_SOURCE_TIFF, ".tiff", "tiff", "TIFFResourceClass" },
    { EDDS_SOURCE_DDS, ".dds", "dds", "DDSResourceClass" },
    { EDDS_SOURCE_HDR, ".hdr", "hdr", "HDRResourceClass" }
};

/** The whole table, with its number of rows in `count` when that is not NULL. */
const edds_source_capability *edds_source_capabilities(size_t *count) {
    if (count != NULL) {
        *count = sizeof capabilities / sizeof capabilities[0];
    }

    return capabilities;
}

/** The row of one source format, or NULL when the table has none. */
const edds_source_capability *edds_source_capability_of_format(edds_source_format format) {
    for (size_t at = 0; at < sizeof capabilities / sizeof capabilities[0]; ++at) {
        if (capabilities[at].format == format) {
            return &capabilities[at];
        }
    }

    return NULL;
}

/** The row whose Workbench resource class is exactly this name, or NULL when none is. */
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
