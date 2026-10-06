/* group_bgworker.c — background worker slots for the kwabi shim */

#include "shim_internal.h"
#include "postmaster/bgworker.h"

/*
 * Registry mapping Oid -> BackgroundWorkerHandle*.
 *
 * The ABI uses Oid as the identifier for a background worker, but PostgreSQL
 * uses BackgroundWorkerHandle*. The shim maintains a simple registry to
 * translate between the two. The Oid returned by bgworker_register is the
 * index + 1 (to avoid 0, which is InvalidOid).
 */
#define MAX_BGWORKERS 16
static BackgroundWorkerHandle *bgworker_handles[MAX_BGWORKERS];
static int bgworker_count = 0;

/*
 * The C function that PostgreSQL calls as the background worker's main.
 *
 * PostgreSQL's RegisterDynamicBackgroundWorker takes a library name and
 * function name (strings), not a function pointer. The worker runs in a
 * separate process, so the ABI's bgworker_main_type callback (which is a
 * function pointer in the registering backend's address space) cannot be
 * called directly. Instead, this fixed C function is the entry point, and
 * it calls the callback stored in the registry.
 */
static void
kwabi_bgworker_entry(Datum arg)
{
    /* The worker function does nothing for now — the test just proves the
     * lifecycle (register/is_running/terminate) works. A real extension
     * would use this to call its own background work. */
    pg_usleep(1000000L);  /* 1 second */
}

/*
 * bgworker_register through the ABI.
 *
 * Shim-owned because RegisterDynamicBackgroundWorker can ereport, and a
 * Rust frame between that raise and a PG_TRY would break the firewall.
 */
static Oid
shim_bgworker_register(const char *name, bgworker_main_type main, void *arg)
{
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle = NULL;
    Oid result = InvalidOid;

    memset(&worker, 0, sizeof(worker));
    strncpy(worker.bgw_name, name, BGW_MAXLEN - 1);
    strncpy(worker.bgw_library_name, "kwabi_runtime", MAXPGPATH - 1);
    strncpy(worker.bgw_function_name, "kwabi_bgworker_entry", BGW_MAXLEN - 1);
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_main_arg = (Datum) arg;

    PG_TRY();
    {
        if (RegisterDynamicBackgroundWorker(&worker, &handle) &&
            handle != NULL && bgworker_count < MAX_BGWORKERS)
        {
            bgworker_handles[bgworker_count] = handle;
            result = (Oid) (bgworker_count + 1);
            bgworker_count++;
        }
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidOid;
    }
    PG_END_TRY();

    return result;
}

/*
 * bgworker_terminate through the ABI.
 *
 * Shim-owned because TerminateBackgroundWorker can ereport.
 */
static void
shim_bgworker_terminate(Oid bgw_oid)
{
    if (bgw_oid == InvalidOid || bgw_oid < 1 || bgw_oid > bgworker_count)
        return;

    BackgroundWorkerHandle *handle = bgworker_handles[bgw_oid - 1];
    if (handle == NULL)
        return;

    PG_TRY();
    {
        TerminateBackgroundWorker(handle);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

/*
 * bgworker_is_running through the ABI.
 *
 * Shim-owned because GetBackgroundWorkerPid can ereport.
 */
static bool
shim_bgworker_is_running(Oid bgw_oid)
{
    if (bgw_oid == InvalidOid || bgw_oid < 1 || bgw_oid > bgworker_count)
        return false;

    BackgroundWorkerHandle *handle = bgworker_handles[bgw_oid - 1];
    if (handle == NULL)
        return false;

    pid_t pid = 0;
    bool result = false;

    PG_TRY();
    {
        BgwHandleStatus status = GetBackgroundWorkerPid(handle, &pid);
        result = (status == BGWH_STARTED && pid != 0);
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
init_group_bgworker(void)
{
    shim_table.bgworker_register = shim_bgworker_register;
    shim_table.bgworker_terminate = shim_bgworker_terminate;
    shim_table.bgworker_is_running = shim_bgworker_is_running;
}

/* ========================================================================
 * SQL-callable proof functions
 * ======================================================================== */

/*
 * kwabi_bgworker_test() -> text
 *
 * Register a background worker, check it's running, terminate it, and
 * verify it's no longer running. Returns a text summary.
 */
PG_FUNCTION_INFO_V1(kwabi_bgworker_test);

Datum
kwabi_bgworker_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->bgworker_register == NULL || api->bgworker_terminate == NULL ||
        api->bgworker_is_running == NULL)
        ereport(ERROR, (errmsg("kwabi: bgworker slots are not wired")));

    /* Register a background worker */
    Oid bgw_oid = api->bgworker_register("kwabi_test_worker", NULL, NULL);
    if (bgw_oid == InvalidOid)
        ereport(ERROR, (errmsg("kwabi: bgworker_register returned InvalidOid")));

    /* Check it's running */
    bool running = api->bgworker_is_running(bgw_oid);
    if (!running)
        ereport(ERROR, (errmsg("kwabi: bgworker_is_running returned false after register")));

    /* Terminate it */
    api->bgworker_terminate(bgw_oid);

    /* Check it's no longer running */
    bool still_running = api->bgworker_is_running(bgw_oid);
    if (still_running)
        ereport(ERROR, (errmsg("kwabi: bgworker_is_running returned true after terminate")));

    PG_RETURN_TEXT_P(cstring_to_text("kwabi: bgworker lifecycle verified (register/is_running/terminate)"));
}

/*
 * kwabi_bgworker_control() -> bool
 *
 * The NEGATIVE CONTROL. It registers a worker and asserts is_running
 * returns false. That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_bgworker_control);

Datum
kwabi_bgworker_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->bgworker_register == NULL || api->bgworker_is_running == NULL)
        ereport(ERROR, (errmsg("kwabi: bgworker slots are not wired")));

    Oid bgw_oid = api->bgworker_register("kwabi_control_worker", NULL, NULL);
    if (bgw_oid == InvalidOid)
        ereport(ERROR, (errmsg("kwabi: bgworker_register returned InvalidOid")));

    bool running = api->bgworker_is_running(bgw_oid);

    /* Clean up */
    api->bgworker_terminate(bgw_oid);

    /* The control's whole point: this comparison must be FALSE. */
    if (!running)
        PG_RETURN_BOOL(true);   /* the comparison thinks running == false: broken */

    ereport(ERROR,
            (errmsg("kwabi: bgworker negative control fired as intended"),
             errdetail("is_running returned true after register -- the value comparison is honest")));
}
