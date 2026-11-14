/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static HMODULE umd_self;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        umd_self = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

HMODULE umd_module(void)
{
    return umd_self;
}

void umd_log_line(const char *line)
{
    OutputDebugStringA(line);
}

void umd_log(const char *format, ...)
{
    char line[512];
    int prefix = snprintf(line, sizeof(line), "viogpu3d_umd: ");
    va_list args;

    va_start(args, format);
    vsnprintf(line + prefix, sizeof(line) - prefix - 1, format, args);
    va_end(args);
    strcat(line, "\n");
    umd_log_line(line);
}

// DXVK built with -Dumd=true logs through this.
int __cdecl DXVK_umd_log_output(const char *line)
{
    umd_log_line(line);
    return 0;
}

HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData)
{
    VIRTIO_WDDM_ADAPTER_INFO info;
    D3DDDICB_QUERYADAPTERINFO query;
    enum umd_adapter_check check;
    struct umd_adapter *adapter;
    HRESULT hr;

    if (pOpenData == NULL || pOpenData->pAdapterCallbacks == NULL ||
        pOpenData->pAdapterCallbacks->pfnQueryAdapterInfoCb == NULL)
    {
        return E_INVALIDARG;
    }

    memset(&info, 0, sizeof(info));
    memset(&query, 0, sizeof(query));
    query.pPrivateDriverData = &info;
    query.PrivateDriverDataSize = sizeof(info);
    hr = pOpenData->pAdapterCallbacks->pfnQueryAdapterInfoCb(pOpenData->hRTAdapter.handle, &query);
    if (FAILED(hr))
    {
        umd_log("OpenAdapter10_2: the kernel driver's adapter info query failed 0x%08lx", (unsigned long)hr);
        return hr;
    }
    check = umd_adapter_info_check(&info, sizeof(info), &info);
    if (check != UMD_ADAPTER_OK)
    {
        umd_log("OpenAdapter10_2: refused: %s", umd_adapter_check_text(check));
        return E_FAIL;
    }

    adapter = (struct umd_adapter *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*adapter));
    if (adapter == NULL)
    {
        return E_OUTOFMEMORY;
    }
    adapter->hRTAdapter = pOpenData->hRTAdapter.handle;
    adapter->pAdapterCallbacks = pOpenData->pAdapterCallbacks;
    adapter->info = info;
    adapter->luid = umd_adapter_luid(&info);
    umd_log("OpenAdapter10_2: adapter LUID %08lx-%08lx, capability sets 0x%llx",
            (unsigned long)adapter->luid.HighPart,
            (unsigned long)adapter->luid.LowPart,
            (unsigned long long)info.CapsetMask);

    hr = tritonOpenAdapter10_2(pOpenData, adapter);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, adapter);
    }
    return hr;
}
