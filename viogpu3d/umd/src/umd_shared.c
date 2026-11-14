/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_shared.h"

static void umd_lock_take(umd_lock *lock)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(lock);
#else
    pthread_mutex_lock(lock);
#endif
}

static void umd_lock_give(umd_lock *lock)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(lock);
#else
    pthread_mutex_unlock(lock);
#endif
}

HRESULT umd_shared_acquire(struct umd_shared *shared, umd_shared_make_fn make, void **object)
{
    HRESULT hr = S_OK;

    *object = NULL;
    umd_lock_take(&shared->lock);
    if (shared->holders == 0)
    {
        shared->object = NULL;
        hr = make(&shared->object);
        if (FAILED(hr))
        {
            shared->object = NULL;
        }
    }
    if (!FAILED(hr))
    {
        shared->holders++;
        *object = shared->object;
    }
    umd_lock_give(&shared->lock);
    return hr;
}

void umd_shared_release(struct umd_shared *shared, umd_shared_free_fn free_object)
{
    umd_lock_take(&shared->lock);
    if (shared->holders > 0)
    {
        shared->holders--;
        if (shared->holders == 0)
        {
            free_object(shared->object);
            shared->object = NULL;
        }
    }
    umd_lock_give(&shared->lock);
}
