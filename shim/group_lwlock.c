/* group_lwlock.c — lwlock slots for the kwabi shim */

#include "shim_internal.h"

static LWLockMode
kwabi_lwlockmode_to_pg(KwabiLWLockMode mode)
{
    switch (mode) {
        case KWABI_LWLOCKMODE_SHARE:     return LW_SHARED;
        case KWABI_LWLOCKMODE_EXCLUSIVE: return LW_EXCLUSIVE;
        case KWABI_LWLOCKMODE_WAIT:      return LW_SHARED;
        default:                         return LW_SHARED;
    }
}

static void
shim_lwlock_acquire(void *lock, KwabiLWLockMode mode)
{
    LWLock *lw = (LWLock *) lock;
    if (lw == NULL)
        return;
    PG_TRY();
    {
        LWLockAcquire(lw, kwabi_lwlockmode_to_pg(mode));
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_lwlock_release(void *lock)
{
    LWLock *lw = (LWLock *) lock;
    if (lw == NULL)
        return;
    PG_TRY();
    {
        LWLockRelease(lw);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static bool
shim_lwlock_held_by_me(void *lock)
{
    LWLock *lw = (LWLock *) lock;
    if (lw == NULL)
        return false;
    return LWLockHeldByMe(lw);
}

static bool
shim_lwlock_cond_acquire(void *lock, KwabiLWLockMode mode)
{
    LWLock *lw = (LWLock *) lock;
    if (lw == NULL)
        return false;
    bool result = false;
    PG_TRY();
    {
        result = LWLockConditionalAcquire(lw, kwabi_lwlockmode_to_pg(mode));
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = false;
    }
    PG_END_TRY();
    return result;
}

void
init_group_lwlock(void)
{
    shim_table.lwlock_acquire = shim_lwlock_acquire;
    shim_table.lwlock_release = shim_lwlock_release;
    shim_table.lwlock_held_by_me = shim_lwlock_held_by_me;
    shim_table.lwlock_cond_acquire = shim_lwlock_cond_acquire;
}
