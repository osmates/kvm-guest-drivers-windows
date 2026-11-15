/*
 * Copyright 2026 OSMates contributors
 * SPDX-License-Identifier: MIT
 */

// The tiled resources tier Triton reports. Free of platform headers, so the driver's host tests build it.

#ifndef TRITON_TILED_H_INCLUDED
#define TRITON_TILED_H_INCLUDED

// D3D_FEATURE_LEVEL values; tritonDDI.c and tritonDDI12.c check them against d3dcommon.h.
#define TRITON_TILED_LEVEL_11_0 0xb000u
#define TRITON_TILED_LEVEL_11_1 0xb100u
#define TRITON_TILED_LEVEL_12_0 0xc000u

// The DDIs stop at tier 3; a host's tier 4 includes it.
#define TRITON_TILED_TIER_MAX 3u

static inline unsigned tritonTiledClamp(unsigned hostTier)
{
    return hostTier < TRITON_TILED_TIER_MAX ? hostTier : TRITON_TILED_TIER_MAX;
}

// Without hostFeatureLevel: tier 2 at a 12_x label (the committed-backing shim covers it), else none.
static inline unsigned tritonTiledTier12(unsigned hostTier, unsigned featureLevel, int hostFeatureLevel)
{
    if (!hostFeatureLevel)
    {
        return featureLevel >= TRITON_TILED_LEVEL_12_0 ? 2u : 0u;
    }
    return tritonTiledClamp(hostTier);
}

// Tier N is the low N bits; DXVK gives tier 1 from 11_0 and tier 2 from 11_1.
static inline unsigned tritonTiledFlags11(unsigned hostTier, unsigned featureLevel, int hostFeatureLevel)
{
    unsigned tier = hostFeatureLevel ? tritonTiledClamp(hostTier) : 0u;

    if (featureLevel < TRITON_TILED_LEVEL_11_0)
    {
        tier = 0u;
    }
    else if (featureLevel < TRITON_TILED_LEVEL_11_1 && tier > 1u)
    {
        tier = 1u;
    }
    return (1u << tier) - 1u;
}

#endif
