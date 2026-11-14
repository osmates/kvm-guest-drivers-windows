/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

// D3D_FEATURE_LEVEL and D3D11DDI_3DPIPELINELEVEL values; umd_backend.c checks them against the SDK and WDK headers.
#define UMD_FEATURE_LEVEL_10_0  0xa000u
#define UMD_FEATURE_LEVEL_10_1  0xa100u
#define UMD_FEATURE_LEVEL_11_0  0xb000u
#define UMD_FEATURE_LEVEL_11_1  0xb100u

#define UMD_PIPELINE_LEVEL_10_0 0u
#define UMD_PIPELINE_LEVEL_10_1 1u
#define UMD_PIPELINE_LEVEL_11_0 2u
#define UMD_PIPELINE_LEVEL_11_1 3u

// 0 for a pipeline level this driver does not offer.
UINT umd_feature_level_from_pipeline(UINT pipelineLevel);

// The levels from maxLevel down to 10_0, highest first, as D3D11CreateDevice takes them.
UINT umd_feature_levels_upto(UINT maxLevel, UINT *levels, UINT capacity);
