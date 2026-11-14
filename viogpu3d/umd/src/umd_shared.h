/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

#ifdef _WIN32
typedef SRWLOCK umd_lock;
#define UMD_LOCK_INIT SRWLOCK_INIT
#else
#include <pthread.h>

typedef pthread_mutex_t umd_lock;
#define UMD_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#endif

struct umd_shared
{
    umd_lock lock;
    void *object;
    UINT holders;
};

typedef HRESULT (*umd_shared_make_fn)(void **object);
typedef void (*umd_shared_free_fn)(void *object);

// make and free_object run under the lock, so an acquire never sees a half-made or freed object.
HRESULT umd_shared_acquire(struct umd_shared *shared, umd_shared_make_fn make, void **object);
void umd_shared_release(struct umd_shared *shared, umd_shared_free_fn free_object);
