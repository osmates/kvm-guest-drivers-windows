/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// The pure parts build for Windows and, for their unit tests, natively on macOS.
#ifdef _WIN32
#include <windows.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint32_t UINT;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef wchar_t WCHAR;

typedef struct _LUID
{
    DWORD LowPart;
    LONG HighPart;
} LUID;

#define TRUE                      1
#define FALSE                     0

#define C_ASSERT(e)               _Static_assert(e, #e)
#define FIELD_OFFSET(type, field) ((LONG)offsetof(type, field))
#endif

#include "../../shared/viogpu3d_uapi.h"
