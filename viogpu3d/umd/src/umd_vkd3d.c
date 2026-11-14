/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd12.h"
#include "umd_d3d12.h"
#include "umd_path.h"

#include <string.h>

static INIT_ONCE umd_vkd3d_once = INIT_ONCE_STATIC_INIT;
// Loaded for the life of the process, as the ICD: its devices outlive any one runtime device.
static HMODULE umd_vkd3d_module;
static struct umd_vkd3d_api umd_vkd3d_table;
static BOOL umd_vkd3d_ready;

static BOOL umd_make_folder(const WCHAR *path)
{
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// vkd3d-proton writes its cache into the current folder unless told otherwise, and this driver runs in every Direct3D
// 12 process: an app's own folder is no place for it.
static void umd_vkd3d_cache_path(void)
{
    WCHAR local[MAX_PATH], folder[MAX_PATH], parent[MAX_PATH];
    DWORD userLength, localLength;
    enum umd_cache_rule rule;
    WCHAR *last;

    userLength = GetEnvironmentVariableW(L"VKD3D_SHADER_CACHE_PATH", NULL, 0);
    localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    rule = umd_vkd3d_cache_rule(userLength > 1,
                                localLength > 0 && localLength < MAX_PATH ? local : NULL,
                                folder,
                                MAX_PATH);
    if (rule == UMD_CACHE_KEEP)
    {
        umd_log("vkd3d-proton's cache: the process's own VKD3D_SHADER_CACHE_PATH");
        return;
    }
    if (rule == UMD_CACHE_FOLDER)
    {
        memcpy(parent, folder, sizeof(parent));
        last = wcsrchr(parent, L'\\');
        if (last != NULL)
        {
            *last = 0;
        }
        if (last != NULL && umd_make_folder(parent) && umd_make_folder(folder) &&
            SetEnvironmentVariableW(L"VKD3D_SHADER_CACHE_PATH", folder))
        {
            umd_log("vkd3d-proton's cache: %ls", folder);
            return;
        }
        umd_log("vkd3d-proton's cache: no folder %ls (error %lu), so none", folder, (unsigned long)GetLastError());
    }
    if (!SetEnvironmentVariableW(L"VKD3D_SHADER_CACHE_PATH", L"0"))
    {
        umd_log("vkd3d-proton's cache: could not turn it off (error %lu)", (unsigned long)GetLastError());
        return;
    }
    umd_log("vkd3d-proton's cache: off");
}

static BOOL umd_vkd3d_proc(const char *name, void **slot)
{
    *slot = (void *)GetProcAddress(umd_vkd3d_module, name);
    if (*slot == NULL)
    {
        umd_log("vkd3d-proton exports no %s", name);
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK umd_vkd3d_load(PINIT_ONCE once, PVOID param, PVOID *context)
{
    const WCHAR *name = umd_vkd3d_name(UMD_X86);
    WCHAR self[MAX_PATH], path[MAX_PATH];
    struct umd_vkd3d_api *t = &umd_vkd3d_table;
    DWORD length;
    BOOL ok = TRUE;

    (void)once;
    (void)param;
    (void)context;
    length = GetModuleFileNameW(umd_module(), self, MAX_PATH);
    if (length == 0 || length == MAX_PATH || !umd_path_beside(self, name, path, MAX_PATH))
    {
        umd_log("no path for %ls beside this driver", name);
        return TRUE;
    }
    umd_vkd3d_cache_path();
    // Its folder and System32 only: it imports system DLLs only.
    umd_vkd3d_module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (umd_vkd3d_module == NULL)
    {
        umd_log("cannot load %ls: error %lu", path, (unsigned long)GetLastError());
        return TRUE;
    }
    ok &= umd_vkd3d_proc("vkd3d_create_instance", (void **)&t->create_instance);
    ok &= umd_vkd3d_proc("vkd3d_instance_decref", (void **)&t->instance_decref);
    ok &= umd_vkd3d_proc("vkd3d_instance_get_vk_instance", (void **)&t->instance_get_vk_instance);
    ok &= umd_vkd3d_proc("vkd3d_create_device", (void **)&t->create_device);
    ok &= umd_vkd3d_proc("vkd3d_get_vk_device", (void **)&t->get_vk_device);
    ok &= umd_vkd3d_proc("vkd3d_get_vk_physical_device", (void **)&t->get_vk_physical_device);
    ok &= umd_vkd3d_proc("vkd3d_instance_from_device", (void **)&t->instance_from_device);
    ok &= umd_vkd3d_proc("vkd3d_acquire_vk_queue", (void **)&t->acquire_vk_queue);
    ok &= umd_vkd3d_proc("vkd3d_release_vk_queue", (void **)&t->release_vk_queue);
    ok &= umd_vkd3d_proc("vkd3d_enqueue_initial_transition", (void **)&t->enqueue_initial_transition);
    ok &= umd_vkd3d_proc("vkd3d_serialize_versioned_root_signature", (void **)&t->serialize_versioned_root_signature);
    ok &= umd_vkd3d_proc("vkd3d_get_vk_format", (void **)&t->get_vk_format);
    if (!ok)
    {
        return TRUE;
    }
    umd_vkd3d_ready = TRUE;
    umd_log("loaded vkd3d-proton %ls", path);
    return TRUE;
}

const struct umd_vkd3d_api *umd_vkd3d(void)
{
    InitOnceExecuteOnce(&umd_vkd3d_once, umd_vkd3d_load, NULL, NULL);
    return umd_vkd3d_ready ? &umd_vkd3d_table : NULL;
}
