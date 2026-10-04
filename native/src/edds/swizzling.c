/* The table of Workbench `Swizzling` values this converter knows, and the lookups into it. */
#include <edds/edds.h>

#include <string.h>

/**
 * One row per swizzle: the value, its Workbench name, its wire name, and whether it writes alpha.
 * Version-pinned importer captures: tests/workbench/swizzle-goldens.json.
 */
static const edds_swizzle_capability capabilities[] = {
    { EDDS_SWIZZLE_NONE, "None", "none", 0 },
    { EDDS_SWIZZLE_TERRAIN_LAYER, "TerrainLayerTexture", "terrain-layer-texture", 0 },
    { EDDS_SWIZZLE_TERRAIN_SUPER, "TerrainSuperTexture", "terrain-super-texture", 0 },
    { EDDS_SWIZZLE_TERRAIN_NORMAL, "TerrainNormalSpecular_SYxX", "terrain-normal-specular-syxx", 1 },
    { EDDS_SWIZZLE_ALPHA_TO_RGB, "AlphaToRGB", "alpha-to-rgb", 1 },
    { EDDS_SWIZZLE_SMDI_TO_GS, "SMDIToGS", "smdi-to-gs", 1 },
    { EDDS_SWIZZLE_NORMAL_NOHQ, "NormalMap_NOHQ", "normal-map-nohq", 1 },
    { EDDS_SWIZZLE_NORMAL_GA, "NormalMapGA", "normal-map-ga", 1 },
    { EDDS_SWIZZLE_NORMAL_SPECULAR, "NormalSpecularMapXYZS", "normal-specular-map-xyzs", 0 },
    { EDDS_SWIZZLE_AMBIENT_SPECULAR, "AmbientSpecularMapGA", "ambient-specular-map-ga", 0 }
};

/** The whole table, with its number of rows in `count` when that is not NULL. */
const edds_swizzle_capability *edds_swizzles(size_t *count) {
    if (count != NULL) {
        *count = sizeof capabilities / sizeof capabilities[0];
    }

    return capabilities;
}

/** The row of one swizzle, or NULL when the table has none. */
const edds_swizzle_capability *edds_swizzle_capability_of(edds_swizzling swizzling) {
    for (size_t at = 0; at < sizeof capabilities / sizeof capabilities[0]; ++at) {
        if (capabilities[at].swizzling == swizzling) {
            return &capabilities[at];
        }
    }

    return NULL;
}

/** The row whose Workbench name is exactly `name`, or NULL when none is. */
const edds_swizzle_capability *edds_swizzle_of_workbench_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    for (size_t at = 0; at < sizeof capabilities / sizeof capabilities[0]; ++at) {
        if (strcmp(capabilities[at].workbench_name, name) == 0) {
            return &capabilities[at];
        }
    }

    return NULL;
}
