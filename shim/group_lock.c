/* group_lock.c — lock slots for the kwabi shim */

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
shim_lock_acquire(void *lock, KwabiLWLockMode mode)
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
shim_lock_release(void *lock)
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
shim_lock_held_by_me(void *lock)
{
    LWLock *lw = (LWLock *) lock;
    if (lw == NULL)
        return false;
    return LWLockHeldByMe(lw);
}

static void
shim_spin_acquire(void *lock)
{
    slock_t *sl = (slock_t *) lock;
    if (sl == NULL)
        return;
    PG_TRY();
    {
        SpinLockAcquire(sl);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_spin_release(void *lock)
{
    slock_t *sl = (slock_t *) lock;
    if (sl == NULL)
        return;
    PG_TRY();
    {
        SpinLockRelease(sl);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_spinlock_acquire(void *lock)
{
    slock_t *sl = (slock_t *) lock;
    if (sl == NULL)
        return;
    PG_TRY();
    {
        SpinLockAcquire(sl);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_spinlock_release(void *lock)
{
    slock_t *sl = (slock_t *) lock;
    if (sl == NULL)
        return;
    PG_TRY();
    {
        SpinLockRelease(sl);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

/* A spinlock records only that it is locked, not who locked it, so "held by me" cannot be
 * answered. Report that through error_get rather than returning a false that looks real. */
static bool
shim_spinlock_held_by_me(void *lock)
{
    (void) lock;
    shim_unsupported(ERRCODE_FEATURE_NOT_SUPPORTED,
                     "kwabi: spinlock_held_by_me is not supported; a spinlock does not record its owner");
    return false;
}

void
init_group_lock(void)
{
    shim_table.lock_acquire = shim_lock_acquire;
    shim_table.lock_release = shim_lock_release;
    shim_table.lock_held_by_me = shim_lock_held_by_me;
    shim_table.spin_acquire = shim_spin_acquire;
    shim_table.spin_release = shim_spin_release;
    shim_table.spinlock_acquire = shim_spinlock_acquire;
    shim_table.spinlock_release = shim_spinlock_release;
    shim_table.spinlock_held_by_me = shim_spinlock_held_by_me;
}

/* ---- SQL-callable test functions ---- */

PG_FUNCTION_INFO_V1(kwabi_lock_test);

Datum
kwabi_lock_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->lock_acquire == NULL || api->lock_release == NULL ||
        api->lock_held_by_me == NULL)
        ereport(ERROR, (errmsg("kwabi: lock slots are not wired")));

    LWLock *lock = (LWLock *) palloc(sizeof(LWLock));
    memset(lock, 0, sizeof(LWLock));

    api->lock_acquire(lock, KWABI_LWLOCKMODE_EXCLUSIVE);

    bool held = api->lock_held_by_me(lock);
    if (!held)
        ereport(ERROR, (errmsg("kwabi: lock_held_by_me returned false after acquire")));

    api->lock_release(lock);

    held = api->lock_held_by_me(lock);
    if (held)
        ereport(ERROR, (errmsg("kwabi: lock_held_by_me returned true after release")));

    pfree(lock);

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(kwabi_spin_test);

Datum
kwabi_spin_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->spin_acquire == NULL || api->spin_release == NULL)
        ereport(ERROR, (errmsg("kwabi: spin slots are not wired")));

    slock_t *lock = (slock_t *) palloc(sizeof(slock_t));
    memset(lock, 0, sizeof(slock_t));

    api->spin_acquire(lock);
    api->spin_release(lock);

    pfree(lock);

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(kwabi_spinlock_test);

Datum
kwabi_spinlock_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->spinlock_acquire == NULL || api->spinlock_release == NULL ||
        api->spinlock_held_by_me == NULL)
        ereport(ERROR, (errmsg("kwabi: spinlock slots are not wired")));

    slock_t *lock = (slock_t *) palloc(sizeof(slock_t));
    memset(lock, 0, sizeof(slock_t));

    api->spinlock_acquire(lock);
    api->spinlock_release(lock);

    pfree(lock);

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(kwabi_lock_control);

Datum
kwabi_lock_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->lock_acquire == NULL || api->lock_release == NULL ||
        api->lock_held_by_me == NULL)
        ereport(ERROR, (errmsg("kwabi: lock slots are not wired")));

    LWLock *lock = (LWLock *) palloc(sizeof(LWLock));
    memset(lock, 0, sizeof(LWLock));

    api->lock_acquire(lock, KWABI_LWLOCKMODE_EXCLUSIVE);

    bool held = api->lock_held_by_me(lock);
    if (!held)
        PG_RETURN_BOOL(true);

    api->lock_release(lock);
    pfree(lock);

    ereport(ERROR,
            (errmsg("kwabi: lock negative control fired as intended"),
             errdetail("lock_held_by_me returned true after acquire -- the comparison is honest")));
}
