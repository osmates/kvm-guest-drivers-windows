/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

#if defined(__i386__) || defined(_M_IX86)
#define UMD_X86 TRUE
#else
#define UMD_X86 FALSE
#endif

// The path of a file next to this module, in the driver store folder the package was installed to.
BOOL umd_path_beside(const WCHAR *modulePath, const WCHAR *name, WCHAR *out, size_t capacity);

// The Venus ICD the package installs beside the driver. One ARM64X DLL serves ARM64 and x64 processes; the x86 one
// shares the folder under its own name.
const WCHAR *umd_icd_name(BOOL x86);
