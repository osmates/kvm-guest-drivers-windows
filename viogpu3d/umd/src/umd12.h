/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd.h"

#include "../triton/triton12.h"

#include "umd_vkd3d_api.h"

// vkd3d-proton's API from the DLL beside this driver, loaded once; NULL when the DLL or one of its functions is
// missing. Sets VKD3D_SHADER_CACHE_PATH first (umd_vkd3d_cache_rule).
const struct umd_vkd3d_api *umd_vkd3d(void);

// Opens the adapter for one of the runtimes' entry points: the kernel driver's adapter info, checked.
HRESULT umd_adapter_open(HANDLE hRTAdapter,
                         const D3DDDI_ADAPTERCALLBACKS *callbacks,
                         const char *entry,
                         struct umd_adapter **out);
