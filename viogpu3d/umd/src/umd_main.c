/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd12.h"

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

static INIT_ONCE umd_log_once = INIT_ONCE_STATIC_INIT;
static HANDLE umd_log_handle = INVALID_HANDLE_VALUE;

// LogonUI and DWM run in a session no debug-output collector can reach, so VIOGPU3D_UMD_LOG, set machine-wide, names a
// folder that gets one file per process.
static BOOL CALLBACK umd_log_open(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    char folder[MAX_PATH];
    char exe[MAX_PATH];
    char path[MAX_PATH];
    (void)once;
    (void)parameter;
    (void)context;
    DWORD length = GetEnvironmentVariableA("VIOGPU3D_UMD_LOG", folder, sizeof(folder));
    if (length == 0 || length >= sizeof(folder))
    {
        return TRUE;
    }
    length = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (length == 0 || length >= sizeof(exe))
    {
        return TRUE;
    }
    const char *name = strrchr(exe, '\\');
    name = name != NULL ? name + 1 : exe;
    int written = snprintf(path, sizeof(path), "%s\\%s-%lu.log", folder, name, (unsigned long)GetCurrentProcessId());
    if (written > 0 && written < (int)sizeof(path))
    {
        umd_log_handle = CreateFileA(path,
                                     FILE_APPEND_DATA,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL,
                                     OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL,
                                     NULL);
    }
    return TRUE;
}

void umd_log_line(const char *line)
{
    OutputDebugStringA(line);
    InitOnceExecuteOnce(&umd_log_once, umd_log_open, NULL, NULL);
    if (umd_log_handle != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(umd_log_handle, line, (DWORD)strlen(line), &written, NULL);
    }
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

HRESULT umd_adapter_open(HANDLE hRTAdapter,
                         const D3DDDI_ADAPTERCALLBACKS *callbacks,
                         const char *entry,
                         struct umd_adapter **out)
{
    VIRTIO_WDDM_ADAPTER_INFO info;
    D3DDDICB_QUERYADAPTERINFO query;
    enum umd_adapter_check check;
    struct umd_adapter *adapter;
    HRESULT hr;

    *out = NULL;
    if (callbacks == NULL || callbacks->pfnQueryAdapterInfoCb == NULL)
    {
        return E_INVALIDARG;
    }
    memset(&info, 0, sizeof(info));
    memset(&query, 0, sizeof(query));
    query.pPrivateDriverData = &info;
    query.PrivateDriverDataSize = sizeof(info);
    hr = callbacks->pfnQueryAdapterInfoCb(hRTAdapter, &query);
    if (FAILED(hr))
    {
        umd_log("%s: the kernel driver's adapter info query failed 0x%08lx", entry, (unsigned long)hr);
        return hr;
    }
    check = umd_adapter_info_check(&info, sizeof(info), &info);
    if (check != UMD_ADAPTER_OK)
    {
        umd_log("%s: refused: %s", entry, umd_adapter_check_text(check));
        return E_FAIL;
    }

    adapter = (struct umd_adapter *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*adapter));
    if (adapter == NULL)
    {
        return E_OUTOFMEMORY;
    }
    adapter->hRTAdapter = hRTAdapter;
    adapter->pAdapterCallbacks = callbacks;
    adapter->info = info;
    adapter->luid = umd_adapter_luid(&info);
    umd_log("%s: adapter LUID %08lx-%08lx, capability sets 0x%llx",
            entry,
            (unsigned long)adapter->luid.HighPart,
            (unsigned long)adapter->luid.LowPart,
            (unsigned long long)info.CapsetMask);
    *out = adapter;
    return S_OK;
}

HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData)
{
    struct umd_adapter *adapter;
    HRESULT hr;

    if (pOpenData == NULL)
    {
        return E_INVALIDARG;
    }
    hr = umd_adapter_open(pOpenData->hRTAdapter.handle, pOpenData->pAdapterCallbacks, "OpenAdapter10_2", &adapter);
    if (FAILED(hr))
    {
        return hr;
    }
    hr = tritonOpenAdapter10_2(pOpenData, adapter);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, adapter);
    }
    return hr;
}

HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *pOpenData)
{
    struct umd_adapter *adapter;
    HRESULT hr;

    if (pOpenData == NULL)
    {
        return E_INVALIDARG;
    }
    hr = umd_adapter_open(pOpenData->hRTAdapter.handle, pOpenData->pAdapterCallbacks, "OpenAdapter12", &adapter);
    if (FAILED(hr))
    {
        return hr;
    }
    hr = tritonOpenAdapter12(pOpenData, adapter);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, adapter);
    }
    return hr;
}
