/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_pipeline.h"

static const UINT umd_levels[] = {
                                                                                                    UMD_FEATURE_LEVEL_11_1,
                                                                                                    UMD_FEATURE_LEVEL_11_0,
                                                                                                    UMD_FEATURE_LEVEL_10_1,
                                                                                                    UMD_FEATURE_LEVEL_10_0,
};

UINT umd_feature_level_from_pipeline(UINT pipelineLevel)
{
    switch (pipelineLevel)
    {
        case UMD_PIPELINE_LEVEL_10_0:
            return UMD_FEATURE_LEVEL_10_0;
        case UMD_PIPELINE_LEVEL_10_1:
            return UMD_FEATURE_LEVEL_10_1;
        case UMD_PIPELINE_LEVEL_11_0:
            return UMD_FEATURE_LEVEL_11_0;
        case UMD_PIPELINE_LEVEL_11_1:
            return UMD_FEATURE_LEVEL_11_1;
    }
    return 0;
}

UINT umd_feature_levels_upto(UINT maxLevel, UINT *levels, UINT capacity)
{
    UINT count = 0;

    for (UINT i = 0; i < sizeof(umd_levels) / sizeof(umd_levels[0]) && count < capacity; i++)
    {
        if (umd_levels[i] <= maxLevel)
        {
            levels[count++] = umd_levels[i];
        }
    }
    return count;
}
