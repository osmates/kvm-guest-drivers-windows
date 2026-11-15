/*
 * Copyright 2026 OSMates contributors
 * SPDX-License-Identifier: MIT
 */

// The feature level Triton reports. Free of platform headers, so the driver's host tests build it.

#ifndef TRITON_LEVEL_H_INCLUDED
#define TRITON_LEVEL_H_INCLUDED

#include "tritonTiled.h"

// D3D11DDI_3DPIPELINELEVEL values; tritonDDI.c checks them against d3d10umddi.h.
#define TRITON_PIPELINE11_10_0 0u
#define TRITON_PIPELINE11_10_1 1u
#define TRITON_PIPELINE11_11_0 2u
#define TRITON_PIPELINE11_11_1 3u
#define TRITON_PIPELINE11_12_0 7u
#define TRITON_PIPELINE11_12_1 8u

// D3D12DDI_3DPIPELINELEVEL values; tritonDDI12.c checks them against d3d12umddi.h.
#define TRITON_PIPELINE12_11_0 10u
#define TRITON_PIPELINE12_11_1 11u
#define TRITON_PIPELINE12_12_0 12u
#define TRITON_PIPELINE12_12_1 13u
#define TRITON_PIPELINE12_12_2 14u

// The DDIs stop at conservative rasterization tier 3.
#define TRITON_CONSERVATIVE_TIER_MAX 3u

// The inner device's answers.
struct triton_level_caps
{
    unsigned level; // its highest D3D_FEATURE_LEVEL, 0 when it gave none
    unsigned tiledTier;
    unsigned resourceBindingTier; // Direct3D 12 only
    unsigned typedUavLoads;       // TypedUAVLoadAdditionalFormats
    unsigned conservativeTier;
    unsigned rovs;
};

// What Triton's Direct3D 12 layer reports.
struct triton_level12
{
    unsigned level; // D3D_FEATURE_LEVEL; tritonPipeline12 gives the pipeline level
    unsigned tiledTier;
    unsigned conservativeTier;
};

static inline struct triton_level12 tritonLevel12(const struct triton_level_caps *inner, int hostFeatureLevel, int dxil)
{
    struct triton_level12 out;
    const unsigned conservative =
        inner->conservativeTier < TRITON_CONSERVATIVE_TIER_MAX ? inner->conservativeTier : TRITON_CONSERVATIVE_TIER_MAX;

    if (!hostFeatureLevel)
    {
        const int fl12_0 = inner->resourceBindingTier >= 2u && inner->typedUavLoads;
        const int fl12_1 = fl12_0 && inner->conservativeTier >= 1u && inner->rovs;

        out.level = dxil || fl12_1 ? TRITON_LEVEL_12_1 : fl12_0 ? TRITON_LEVEL_12_0 : TRITON_LEVEL_11_1;
        out.tiledTier = tritonTiledTier12(inner->tiledTier, out.level, 0);
        // The 12_1 label claims tier 1 with DXIL: a PSO that turns it on fails at create.
        out.conservativeTier = dxil ? 1u : out.level >= TRITON_LEVEL_12_1 ? conservative : 0u;
        return out;
    }
    out.tiledTier = tritonTiledTier12(inner->tiledTier, inner->level, 1);
    out.conservativeTier = conservative;
    out.level = inner->level >= TRITON_LEVEL_11_1 ? inner->level : TRITON_LEVEL_11_0;
    // 12_2 needs raytracing, variable rate shading and sampler feedback, which are stubs.
    if (out.level > TRITON_LEVEL_12_1)
    {
        out.level = TRITON_LEVEL_12_1;
    }
    if (out.level >= TRITON_LEVEL_12_1 && !(out.conservativeTier >= 1u && inner->rovs))
    {
        out.level = TRITON_LEVEL_12_0;
    }
    if (out.level >= TRITON_LEVEL_12_0 &&
        !(out.tiledTier >= 2u && inner->resourceBindingTier >= 2u && inner->typedUavLoads))
    {
        out.level = TRITON_LEVEL_11_1;
    }
    return out;
}

// A ceiling below 11_0 sets no limit: 0, or one of the runtime's 1_0 core levels.
static inline unsigned tritonPipeline12(unsigned level, unsigned ceiling)
{
    unsigned pipeline = level >= TRITON_LEVEL_12_2   ? TRITON_PIPELINE12_12_2
                        : level >= TRITON_LEVEL_12_1 ? TRITON_PIPELINE12_12_1
                        : level >= TRITON_LEVEL_12_0 ? TRITON_PIPELINE12_12_0
                        : level >= TRITON_LEVEL_11_1 ? TRITON_PIPELINE12_11_1
                                                     : TRITON_PIPELINE12_11_0;

    if (ceiling >= TRITON_PIPELINE12_11_0 && pipeline > ceiling)
    {
        pipeline = ceiling;
    }
    return pipeline;
}

static inline unsigned tritonLevel11(const struct triton_level_caps *inner, int hostFeatureLevel, int wddm2_0)
{
    const unsigned tiledFlags = tritonTiledFlags11(inner->tiledTier, inner->level, hostFeatureLevel);
    unsigned level = inner->level;

    if (level > TRITON_LEVEL_12_1)
    {
        level = TRITON_LEVEL_12_1;
    }
    // The 12_x pipeline levels are WDDM 2.0 DDI values.
    if (!wddm2_0 && level > TRITON_LEVEL_11_1)
    {
        level = TRITON_LEVEL_11_1;
    }
    if (level >= TRITON_LEVEL_12_1 && !(inner->conservativeTier >= 1u && inner->rovs))
    {
        level = TRITON_LEVEL_12_0;
    }
    if (level >= TRITON_LEVEL_12_0 && !((tiledFlags & 2u) && inner->typedUavLoads))
    {
        level = TRITON_LEVEL_11_1;
    }
    return level;
}

// D3D11DDICAPS_3DPIPELINESUPPORT: 10_0 and every level above it up to level, as the runtime takes no gap.
static inline unsigned tritonPipelineCaps11(unsigned level)
{
    unsigned caps = 1u << TRITON_PIPELINE11_10_0;

    caps |= level >= TRITON_LEVEL_10_1 ? 1u << TRITON_PIPELINE11_10_1 : 0u;
    caps |= level >= TRITON_LEVEL_11_0 ? 1u << TRITON_PIPELINE11_11_0 : 0u;
    caps |= level >= TRITON_LEVEL_11_1 ? 1u << TRITON_PIPELINE11_11_1 : 0u;
    caps |= level >= TRITON_LEVEL_12_0 ? 1u << TRITON_PIPELINE11_12_0 : 0u;
    caps |= level >= TRITON_LEVEL_12_1 ? 1u << TRITON_PIPELINE11_12_1 : 0u;
    return caps;
}

// The level a Direct3D 11 CreateDevice asks for; 0 for a pipeline level Triton does not offer.
static inline unsigned tritonLevelFromPipeline11(unsigned pipeline)
{
    switch (pipeline)
    {
        case TRITON_PIPELINE11_10_0:
            return TRITON_LEVEL_10_0;
        case TRITON_PIPELINE11_10_1:
            return TRITON_LEVEL_10_1;
        case TRITON_PIPELINE11_11_0:
            return TRITON_LEVEL_11_0;
        case TRITON_PIPELINE11_11_1:
            return TRITON_LEVEL_11_1;
        case TRITON_PIPELINE11_12_0:
            return TRITON_LEVEL_12_0;
        case TRITON_PIPELINE11_12_1:
            return TRITON_LEVEL_12_1;
    }
    return 0u;
}

#endif
