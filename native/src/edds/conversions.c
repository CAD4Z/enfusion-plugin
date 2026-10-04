/*
 * The one table of Workbench `Conversion` values this converter knows. Adding a conversion is
 * adding a row here and an encoder behind it; the CLI, the batch protocol, the metadata reader and
 * the metadata writer all read this table instead of carrying a list of their own.
 *
 * The GPU format each row produces is settled in `convert.c`. The mapping is not invented: DayZ's
 * own texture corpus pairs each `.edds.meta` recipe with the `.edds` Workbench wrote from it, and
 * `DXTCompression`, `RedHQCompression`, `RedGreenHQCompression` and `ColorHQCompression` are read
 * off those pairs. `Red` and `RedGreen` have no pair in the corpus and take the engine's own
 * single- and two-channel formats; see the ADR for exactly how far the evidence goes.
 */
#include <edds/edds.h>

#include <string.h>

static const edds_conversion_capability capabilities[] = {
    { EDDS_CONVERSION_NONE, "None", "none", 1, 0 },
    { EDDS_CONVERSION_DXT, "DXTCompression", "dxt-compression", 1, 1 },
    { EDDS_CONVERSION_RED, "Red", "red", 1, 0 },
    { EDDS_CONVERSION_RED_HQ, "RedHQCompression", "red-hq-compression", 1, 1 },
    { EDDS_CONVERSION_RED_GREEN, "RedGreen", "red-green", 1, 0 },
    { EDDS_CONVERSION_RED_GREEN_HQ, "RedGreenHQCompression", "red-green-hq-compression", 1, 1 },
    { EDDS_CONVERSION_COLOR_HQ, "ColorHQCompression", "color-hq-compression", 1, 1 },
    /* BC6H and the HDR source pipeline behind it are their own slice; recognized, never guessed. */
    { EDDS_CONVERSION_HDR, "HDRCompression", "hdr-compression", 0, 1 }
};

enum {
    CAPABILITY_COUNT = sizeof capabilities / sizeof capabilities[0]
};

const edds_conversion_capability *edds_conversions(size_t *count) {
    if (count != NULL) {
        *count = CAPABILITY_COUNT;
    }
    return capabilities;
}

const edds_conversion_capability *edds_conversion_capability_of(edds_conversion conversion) {
    for (size_t at = 0; at < CAPABILITY_COUNT; ++at) {
        if (capabilities[at].conversion == conversion) {
            return &capabilities[at];
        }
    }
    return NULL;
}

const edds_conversion_capability *edds_conversion_of_workbench_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t at = 0; at < CAPABILITY_COUNT; ++at) {
        if (strcmp(capabilities[at].workbench_name, name) == 0) {
            return &capabilities[at];
        }
    }
    return NULL;
}

const edds_conversion_capability *edds_conversion_of_wire_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t at = 0; at < CAPABILITY_COUNT; ++at) {
        if (strcmp(capabilities[at].wire_name, name) == 0) {
            return &capabilities[at];
        }
    }
    return NULL;
}
