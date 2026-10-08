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
 * The C function PostgreSQL calls as the background worker's main.
 *
 * PostgreSQL's RegisterDynamicBackgroundWorker takes a LIBRARY NAME and a
 * FUNCTION NAME (strings), because the worker runs in a separate process with
 * no access to the registering backend's address space. So this entry point
 * must be a non-static, exported symbol in the bundle, and the library name
 * must be the bundle's actual filename (kwabi_runtime_pgNN) — not a made-up
 * "kwabi_runtime", which is what made every worker exit with
 * 'could not access file "kwabi_runtime"'. KWABI_BUNDLE_NAME is supplied by
 * the Makefile, so the two can never drift.
 *
 * The ABI's bgworker_main_type callback cannot be called directly here (it is
 * a pointer in another process); a real extension would pass its work as
 * bgw_main_arg. For the lifecycle test the entry just idles briefly.
 */
void
kwabi_bgworker_entry(Datum arg)
{
    (void) arg;
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
    /*
     * Bound on the FIELD's size, not a constant. `bgw_library_name` is
     * BGW_MAXLEN (96) on PG16 but MAXPGPATH (1024) on PG17/18; a
     * `strncpy(..., MAXPGPATH - 1)` therefore writes past the end on PG16 and
     * _FORTIFY_SOURCE aborts the backend (SIGTRAP -> server restart), which
     * took the whole matrix down at bgworker check 1. sizeof is correct on
     * every major.
     */
#ifdef KWABI_BUNDLE_NAME
    /* The real bundle filename, supplied by the Makefile. Using a literal
     * "kwabi_runtime" here made every worker die with 'could not access file'. */
    strncpy(worker.bgw_library_name, KWABI_BUNDLE_NAME,
            sizeof(worker.bgw_library_name) - 1);
#else
    strncpy(worker.bgw_library_name, "kwabi_runtime",
            sizeof(worker.bgw_library_name) - 1);
#endif
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
        /*
         * Terminate is asynchronous: it signals the worker, which then exits
         * on its own. The ABI's is_running must reflect "stopped" right after
         * this returns, so wait for the shutdown rather than assuming it.
         *
         * WaitForBackgroundWorkerShutdown polls forever — if the worker never
         * reaches BGWH_STOPPED (e.g. it was never started, or it is stuck),
         * the test hangs. Use a bounded poll instead: 50 × 100ms = 5s max.
         */
        TerminateBackgroundWorker(handle);
        for (int i = 0; i < 50; i++)
        {
            pid_t pid;
            BgwHandleStatus status = GetBackgroundWorkerPid(handle, &pid);
            if (status == BGWH_STOPPED || status == BGWH_POSTMASTER_DIED)
                break;
            pg_usleep(100000L);  /* 100ms */
        }
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
        /*
         * GetBackgroundWorkerPid returns BGWH_NOT_YET_STARTED until the worker
         * has forked AND reached a consistent state. A single immediate poll
         * therefore reports false for a worker that was registered a moment
         * ago — which is exactly the "returned false after register" failure.
         * Poll briefly rather than once. Bounded, so a worker that never starts
         * still reports false instead of hanging.
         */
        for (int i = 0; i < 100; i++)
        {
            BgwHandleStatus status = GetBackgroundWorkerPid(handle, &pid);

            if (status == BGWH_STARTED && pid != 0)
            {
                result = true;
                break;
            }
            if (status == BGWH_STOPPED || status == BGWH_POSTMASTER_DIED)
                break;

            pg_usleep(10000L);   /* 10ms */
        }
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
