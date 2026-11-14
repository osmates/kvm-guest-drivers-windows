/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

// The path of a file next to this module, in the driver store folder the package was installed to.
BOOL umd_path_beside(const WCHAR *modulePath, const WCHAR *name, WCHAR *out, size_t capacity);
