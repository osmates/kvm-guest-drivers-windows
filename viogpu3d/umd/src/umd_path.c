/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_path.h"

static size_t umd_wcslen(const WCHAR *s)
{
    size_t n = 0;

    while (s[n] != 0)
    {
        n++;
    }
    return n;
}

BOOL umd_path_beside(const WCHAR *modulePath, const WCHAR *name, WCHAR *out, size_t capacity)
{
    size_t dir = 0, nameLength;

    if (modulePath == NULL || name == NULL || out == NULL || name[0] == 0)
    {
        return FALSE;
    }
    for (size_t i = 0; modulePath[i] != 0; i++)
    {
        if (modulePath[i] == L'\\' || modulePath[i] == L'/')
        {
            dir = i + 1;
        }
    }
    if (dir == 0)
    {
        return FALSE;
    }
    for (size_t i = 0; name[i] != 0; i++)
    {
        if (name[i] == L'\\' || name[i] == L'/')
        {
            return FALSE;
        }
    }
    nameLength = umd_wcslen(name);
    if (dir + nameLength + 1 > capacity)
    {
        return FALSE;
    }
    for (size_t i = 0; i < dir; i++)
    {
        out[i] = modulePath[i];
    }
    for (size_t i = 0; i <= nameLength; i++)
    {
        out[dir + i] = name[i];
    }
    return TRUE;
}

const WCHAR *umd_icd_name(BOOL x86)
{
    return x86 ? L"vulkan_virtio_x86.dll" : L"vulkan_virtio.dll";
}
