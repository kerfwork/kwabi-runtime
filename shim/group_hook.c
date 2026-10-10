/* group_hook.c — executor hook chain for the kwabi shim (kwabi.h "Executor hooks").
 *
 * PostgreSQL calls a trampoline as its executor hook. The trampoline runs the
 * registered bodies in order, the first registered outermost. Each body receives
 * a `next` handle; calling hook_next_* runs the rest of the chain, and the last
 * link is the standard executor function (or the non-kwabi hook that was installed
 * before ours).
 *
 * The rules this file keeps, from notes/hook-registry-design.md:
 *   - no PG_TRY around a body, and no body is called from inside a PG_TRY;
 *   - a body reports failure through its return value and `err`; it never raises;
 *   - the only PG_TRY is in a standard link, around PostgreSQL code;
 *   - the trampoline raises, from C, after the chain has returned.
 *
 * Bodies are not in a subtransaction. A failed body fails the statement, and the
 * statement's own abort undoes whatever the body wrote.
 */

#include "shim_internal.h"
#include "executor/executor.h"
#include "optimizer/planner.h"
#include "tcop/utility.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include <dlfcn.h>
#include <string.h>
#include "utils/builtins.h"

#define KWABI_HOOK_MAX_BODIES 8

typedef enum HookPoint
{
    HOOK_START = 0,
    HOOK_RUN,
    HOOK_FINISH,
    HOOK_END,
    HOOK_CHECK_PERMS,
    HOOK_PLANNER,
    HOOK_UTILITY,
    HOOK_NPOINTS
} HookPoint;

typedef struct HookBody
{
    void       *fn;                 /* the body, cast to its point's typedef */
    void       *arg;
} HookBody;

/* The `next` handle. It lives on the stack of the call that created it, so it is
 * valid exactly while the body runs. */
typedef struct HookLink
{
    int         index;              /* the body that was called; next runs index + 1 */
    bool        once;               /* ExecutorRun's execute_once, forwarded unchanged */
} HookLink;

/* Positions 0 .. KWABI_HOOK_NAMED_MAX-1 hold reloadable bodies, one per bound name, in
 * bind order. Unnamed registrations follow them. A chain runs the populated
 * positions in order and skips the empty ones. */
#define KWABI_HOOK_NAMED_MAX 8
#define KWABI_HOOK_NAME_MAX  64

static HookBody bodies[HOOK_NPOINTS][KWABI_HOOK_NAMED_MAX + KWABI_HOOK_MAX_BODIES];
static int  nbodies[HOOK_NPOINTS];      /* one past the last used position */
static bool installed[HOOK_NPOINTS];
static void install_point(HookPoint point);

/* The next populated position at or after `from`, or -1. */
static int
next_present(HookPoint p, int from)
{
    for (int i = from; i < nbodies[p]; i++)
        if (bodies[p][i].fn != NULL)
            return i;
    return -1;
}

/* The hook each point had before we installed ours. Null means none. */
static ExecutorStart_hook_type prev_start = NULL;
static ExecutorRun_hook_type prev_run = NULL;
static ExecutorFinish_hook_type prev_finish = NULL;
static ExecutorEnd_hook_type prev_end = NULL;
static ExecutorCheckPerms_hook_type prev_check_perms = NULL;
static planner_hook_type prev_planner = NULL;
static ProcessUtility_hook_type prev_utility = NULL;

/* ========================================================================
 * Errors: a PostgreSQL ErrorData becomes a KwabiError, and a KwabiError becomes
 * an ereport. Both directions go through the error channel, so detail and hint
 * survive. Object names are carried into the KwabiError but not back out.
 * ======================================================================== */

static void
error_from_edata(KwabiError *err, ErrorData *edata)
{
    kwabi_error_init(err);
    kwabi_error_set_core(err, edata->sqlerrcode, KWABI_ERR_RAISED,
                         edata->message != NULL ? edata->message
                         : "kwabi: PostgreSQL raised without a message");
    kwabi_error_set_detail(err, edata->detail, edata->hint);
    kwabi_error_set_object(err, edata->schema_name, edata->table_name,
                           edata->column_name, edata->datatype_name,
                           edata->constraint_name);
}

/* Raise a KwabiError from C. Only ever called from a trampoline, never from a body. */
static void
raise_from_error(const KwabiError *err)
{
    int         sqlstate = err->sqlerrcode != 0 ? err->sqlerrcode
                                                : ERRCODE_RAISE_EXCEPTION;
    const char *message = err->message[0] != '\0' ? err->message
                                                  : "kwabi hook body failed";

    if (err->detail[0] != '\0' && err->hint[0] != '\0')
        ereport(ERROR,
                (errcode(sqlstate), errmsg("%s", message),
                 errdetail("%s", err->detail), errhint("%s", err->hint)));
    else if (err->detail[0] != '\0')
        ereport(ERROR,
                (errcode(sqlstate), errmsg("%s", message),
                 errdetail("%s", err->detail)));
    else if (err->hint[0] != '\0')
        ereport(ERROR,
                (errcode(sqlstate), errmsg("%s", message),
                 errhint("%s", err->hint)));
    else
        ereport(ERROR, (errcode(sqlstate), errmsg("%s", message)));
}

/* ========================================================================
 * Standard links: the end of each chain. Each one runs the hook that was there
 * before ours, or the standard function, under a PG_TRY that turns an error into
 * KWABI_ERR_RAISED. The PG_TRY holds no body.
 * ======================================================================== */

static KwabiStatus
standard_start(QueryDesc *qd, int eflags, KwabiError *err)
{
    MemoryContext volatile oldcxt = CurrentMemoryContext;
    KwabiStatus volatile status = KWABI_OK;

    PG_TRY();
    {
        if (prev_start)
            prev_start(qd, eflags);
        else
            standard_ExecutorStart(qd, eflags);
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        error_from_edata(err, edata);
        FreeErrorData(edata);
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
standard_run(QueryDesc *qd, int direction, uint64_t count, bool once, KwabiError *err)
{
    MemoryContext volatile oldcxt = CurrentMemoryContext;
    KwabiStatus volatile status = KWABI_OK;

    PG_TRY();
    {
#if PG_VERSION_NUM >= 180000
        (void) once;
        if (prev_run)
            prev_run(qd, (ScanDirection) direction, count);
        else
            standard_ExecutorRun(qd, (ScanDirection) direction, count);
#else
        if (prev_run)
            prev_run(qd, (ScanDirection) direction, count, once);
        else
            standard_ExecutorRun(qd, (ScanDirection) direction, count, once);
#endif
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        error_from_edata(err, edata);
        FreeErrorData(edata);
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
standard_finish(QueryDesc *qd, KwabiError *err)
{
    MemoryContext volatile oldcxt = CurrentMemoryContext;
    KwabiStatus volatile status = KWABI_OK;

    PG_TRY();
    {
        if (prev_finish)
            prev_finish(qd);
        else
            standard_ExecutorFinish(qd);
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        error_from_edata(err, edata);
        FreeErrorData(edata);
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
standard_end(QueryDesc *qd, KwabiError *err)
{
    MemoryContext volatile oldcxt = CurrentMemoryContext;
    KwabiStatus volatile status = KWABI_OK;

    PG_TRY();
    {
        if (prev_end)
            prev_end(qd);
        else
            standard_ExecutorEnd(qd);
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        error_from_edata(err, edata);
        FreeErrorData(edata);
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
standard_check_perms(List *rangeTable, List *rtePermInfos, bool ereport_on_violation,
                     int *allowed, KwabiError *err)
{
    MemoryContext volatile oldcxt = CurrentMemoryContext;
    KwabiStatus volatile status = KWABI_OK;

    /* PostgreSQL's built-in checks have already passed when a hook runs, so with no
     * other hook installed the answer is yes. */
    *allowed = 1;
    PG_TRY();
    {
        if (prev_check_perms)
            *allowed = prev_check_perms(rangeTable, rtePermInfos, ereport_on_violation) ? 1 : 0;
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        error_from_edata(err, edata);
        FreeErrorData(edata);
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
planner_link(Query *parse, const char *query_string, int cursorOptions,
                 ParamListInfo boundParams, PlannedStmt **planned, KwabiError *err)
{
    MemoryContext volatile oldcxt = CurrentMemoryContext;
    KwabiStatus volatile status = KWABI_OK;

    *planned = NULL;
    PG_TRY();
    {
        if (prev_planner)
            *planned = prev_planner(parse, query_string, cursorOptions, boundParams);
        else
            *planned = standard_planner(parse, query_string, cursorOptions, boundParams);
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        error_from_edata(err, edata);
        FreeErrorData(edata);
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
utility_link(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
             ProcessUtilityContext context, ParamListInfo params,
             QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc,
             KwabiError *err)
{
    MemoryContext volatile oldcxt = CurrentMemoryContext;
    KwabiStatus volatile status = KWABI_OK;

    PG_TRY();
    {
        if (prev_utility)
            prev_utility(pstmt, queryString, readOnlyTree, context, params,
                         queryEnv, dest, qc);
        else
            standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
                                    queryEnv, dest, qc);
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        error_from_edata(err, edata);
        FreeErrorData(edata);
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

/* ========================================================================
 * Chains. chain_X(index, ...) runs body `index`, or the standard link once the
 * registered bodies are exhausted. The `next` handle a body receives is a HookLink
 * that names the body it was given to; hook_next_X continues from index + 1.
 * ======================================================================== */

static KwabiStatus
chain_start(int index, QueryDesc *qd, int eflags, KwabiError *err)
{
    {
        int         j = next_present(HOOK_START, index);
        if (j >= 0)
        {
            HookLink    link = {j, false};
            HookBody   *b = &bodies[HOOK_START][j];

            return ((KwabiExecutorStartBody) b->fn)((KwabiQueryDesc) qd, eflags,
                                                    (KwabiHookNext) &link, err, b->arg);
        }
    }
    return standard_start(qd, eflags, err);
}

static KwabiStatus
chain_run(int index, QueryDesc *qd, int direction, uint64_t count, bool once, KwabiError *err)
{
    {
        int         j = next_present(HOOK_RUN, index);
        if (j >= 0)
        {
            HookLink    link = {j, once};
            HookBody   *b = &bodies[HOOK_RUN][j];

            return ((KwabiExecutorRunBody) b->fn)((KwabiQueryDesc) qd, direction, count,
                                                  (KwabiHookNext) &link, err, b->arg);
        }
    }
    return standard_run(qd, direction, count, once, err);
}

static KwabiStatus
chain_finish(int index, QueryDesc *qd, KwabiError *err)
{
    {
        int         j = next_present(HOOK_FINISH, index);
        if (j >= 0)
        {
            HookLink    link = {j, false};
            HookBody   *b = &bodies[HOOK_FINISH][j];

            return ((KwabiExecutorFinishBody) b->fn)((KwabiQueryDesc) qd,
                                                     (KwabiHookNext) &link, err, b->arg);
        }
    }
    return standard_finish(qd, err);
}

static KwabiStatus
chain_end(int index, QueryDesc *qd, KwabiError *err)
{
    {
        int         j = next_present(HOOK_END, index);
        if (j >= 0)
        {
            HookLink    link = {j, false};
            HookBody   *b = &bodies[HOOK_END][j];

            return ((KwabiExecutorEndBody) b->fn)((KwabiQueryDesc) qd,
                                                  (KwabiHookNext) &link, err, b->arg);
        }
    }
    return standard_end(qd, err);
}

static KwabiStatus
chain_check_perms(int index, List *rangeTable, List *rtePermInfos,
                  bool ereport_on_violation, int *allowed, KwabiError *err)
{
    {
        int         j = next_present(HOOK_CHECK_PERMS, index);
        if (j >= 0)
        {
            HookLink    link = {j, false};
            HookBody   *b = &bodies[HOOK_CHECK_PERMS][j];

            return ((KwabiExecutorCheckPermsBody) b->fn)((KwabiList) rangeTable,
                                                         (KwabiList) rtePermInfos,
                                                         ereport_on_violation,
                                                         (KwabiHookNext) &link,
                                                         allowed, err, b->arg);
        }
    }
    return standard_check_perms(rangeTable, rtePermInfos, ereport_on_violation, allowed, err);
}

static KwabiStatus
chain_planner(int index, Query *parse, const char *query_string, int cursorOptions,
              ParamListInfo boundParams, PlannedStmt **planned, KwabiError *err)
{
    {
        int         j = next_present(HOOK_PLANNER, index);
        if (j >= 0)
        {
            HookLink    link = {j, false};
            HookBody   *b = &bodies[HOOK_PLANNER][j];

            return ((KwabiPlannerBody) b->fn)((KwabiNode) parse, query_string, cursorOptions,
                                              (KwabiParamListInfo) boundParams,
                                              (KwabiHookNext) &link,
                                              (KwabiNode *) planned, err, b->arg);
        }
    }
    return planner_link(parse, query_string, cursorOptions, boundParams, planned, err);
}

static KwabiStatus
chain_utility(int index, PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
              ProcessUtilityContext context, ParamListInfo params,
              QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc,
              KwabiError *err)
{
    {
        int         j = next_present(HOOK_UTILITY, index);
        if (j >= 0)
        {
            HookLink    link = {j, false};
            HookBody   *b = &bodies[HOOK_UTILITY][j];

            return ((KwabiProcessUtilityBody) b->fn)((KwabiNode) pstmt, queryString,
                                                     readOnlyTree ? 1 : 0, (int) context,
                                                     (KwabiParamListInfo) params,
                                                     (KwabiQueryEnvironment) queryEnv,
                                                     (KwabiDestReceiver) dest,
                                                     (KwabiQueryCompletion) qc,
                                                     (KwabiHookNext) &link, err, b->arg);
        }
    }
    return utility_link(pstmt, queryString, readOnlyTree, context, params,
                        queryEnv, dest, qc, err);
}

/* ========================================================================
 * Reloadable bodies. The name table lives in shared memory, so a bind is seen by
 * every backend. Each backend keeps its own handles and loads a library the first
 * time it sees a new generation for a name. Old handles are never closed, so a call
 * already running keeps its code. See notes/hook-registry-design.md section 4.
 * ======================================================================== */

typedef struct NamedEntry
{
    char        name[KWABI_HOOK_NAME_MAX];
    char        path[MAXPGPATH];
    uint64      generation;
} NamedEntry;

typedef struct NamedShm
{
    slock_t     mutex;
    int         nentries;
    NamedEntry  e[KWABI_HOOK_NAMED_MAX];
} NamedShm;

static NamedShm *named_shm = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Per backend. */
static uint64 named_seen[KWABI_HOOK_NAMED_MAX];
static void *named_handle[KWABI_HOOK_NAMED_MAX];

static void
clear_named_position(int i)
{
    for (int p = 0; p < HOOK_NPOINTS; p++)
    {
        bodies[p][i].fn = NULL;
        bodies[p][i].arg = NULL;
    }
}

/* Type bodies, one per name position, valid when input is set. */
static KwabiTypeBodies named_types[KWABI_HOOK_NAMED_MAX];
static KwabiAggBodies named_aggs[KWABI_HOOK_NAMED_MAX];

/*
 * Load the library for one name into its positions. A library may supply hook bodies,
 * type bodies, or both; it must supply one of them. A failure leaves the previous
 * bodies in place, and the failure is reported as a warning.
 */
static void
load_named(int i, const char *path)
{
    void       *h;
    KwabiHookBodies *(*hook_getter) (void);
    KwabiTypeBodies *(*type_getter) (void);
    KwabiAggBodies *(*agg_getter) (void);
    bool        (*ext_init) (const KwabiV1 *);
    const KwabiHookBodies *t = NULL;
    const KwabiTypeBodies *tt = NULL;
    const KwabiAggBodies *at = NULL;

    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL)
    {
        ereport(WARNING, (errmsg("kwabi hook bind: %s", dlerror())));
        return;
    }
    hook_getter = (KwabiHookBodies *(*) (void)) dlsym(h, KWABI_HOOK_BODIES_SYMBOL);
    type_getter = (KwabiTypeBodies *(*) (void)) dlsym(h, KWABI_TYPE_BODIES_SYMBOL);
    agg_getter = (KwabiAggBodies *(*) (void)) dlsym(h, KWABI_AGG_BODIES_SYMBOL);
    if (hook_getter == NULL && type_getter == NULL && agg_getter == NULL)
    {
        ereport(WARNING, (errmsg("kwabi bind: %s exports no body table", path)));
        return;
    }
    if (hook_getter != NULL)
    {
        t = hook_getter();
        if (t == NULL || t->size != sizeof(KwabiHookBodies) ||
            t->version != KWABI_HOOK_BODIES_VERSION)
        {
            ereport(WARNING, (errmsg("kwabi hook bind: %s has an incompatible body table", path)));
            return;
        }
    }
    if (type_getter != NULL)
    {
        tt = (const KwabiTypeBodies *) type_getter();
        if (tt == NULL || tt->size != sizeof(KwabiTypeBodies) ||
            tt->version != KWABI_TYPE_BODIES_VERSION)
        {
            ereport(WARNING, (errmsg("kwabi bind: %s has an incompatible type table", path)));
            return;
        }
    }
    if (agg_getter != NULL)
    {
        at = agg_getter();
        if (at == NULL || at->size != sizeof(KwabiAggBodies) ||
            at->version != KWABI_AGG_BODIES_VERSION)
        {
            ereport(WARNING, (errmsg("kwabi bind: %s has an incompatible aggregate table", path)));
            return;
        }
    }
    ext_init = (bool (*) (const KwabiV1 *)) dlsym(h, "kwabi_ext_init");
    if (ext_init != NULL && !ext_init(shim_api))
    {
        ereport(WARNING, (errmsg("kwabi bind: %s kwabi_ext_init failed", path)));
        return;
    }

    clear_named_position(i);
    named_handle[i] = h;
    if (t != NULL)
    {
        bodies[HOOK_START][i].fn = (void *) t->start;
        bodies[HOOK_RUN][i].fn = (void *) t->run;
        bodies[HOOK_FINISH][i].fn = (void *) t->finish;
        bodies[HOOK_END][i].fn = (void *) t->end;
        bodies[HOOK_CHECK_PERMS][i].fn = (void *) t->check_perms;
        bodies[HOOK_PLANNER][i].fn = (void *) t->planner;
        bodies[HOOK_UTILITY][i].fn = (void *) t->utility;
        for (int p = 0; p < HOOK_NPOINTS; p++)
        {
            bodies[p][i].arg = t->arg;
            if (bodies[p][i].fn != NULL)
                install_point((HookPoint) p);
        }
    }
    named_types[i] = (tt != NULL) ? *tt : (KwabiTypeBodies) {0};
    named_aggs[i] = (at != NULL) ? *at : (KwabiAggBodies) {0};
}

/* Bring this backend up to date with the name table. Called on every hook entry. */
static void
sync_named(void)
{
    uint64      gens[KWABI_HOOK_NAMED_MAX];
    int         n;

    if (named_shm == NULL)
        return;

    SpinLockAcquire(&named_shm->mutex);
    n = named_shm->nentries;
    for (int i = 0; i < n; i++)
        gens[i] = named_shm->e[i].generation;
    SpinLockRelease(&named_shm->mutex);

    for (int i = 0; i < n; i++)
    {
        char        path[MAXPGPATH];

        if (gens[i] == named_seen[i])
            continue;
        named_seen[i] = gens[i];

        SpinLockAcquire(&named_shm->mutex);
        strlcpy(path, named_shm->e[i].path, sizeof(path));
        SpinLockRelease(&named_shm->mutex);

        load_named(i, path);
    }
}

/*
 * Check that a library can be loaded and has a table this runtime understands, before
 * it is published. A published path that fails would leave every backend that starts
 * later without a body, so the bind refuses it here instead.
 */
static bool
probe_library(const char *path)
{
    void       *h;
    void       *hook_getter;
    void       *type_getter;
    void       *agg_getter;

    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL)
    {
        ereport(WARNING, (errmsg("kwabi bind refused: %s", dlerror())));
        return false;
    }
    hook_getter = dlsym(h, KWABI_HOOK_BODIES_SYMBOL);
    type_getter = dlsym(h, KWABI_TYPE_BODIES_SYMBOL);
    agg_getter = dlsym(h, KWABI_AGG_BODIES_SYMBOL);
    if (hook_getter == NULL && type_getter == NULL && agg_getter == NULL)
    {
        ereport(WARNING, (errmsg("kwabi bind refused: %s exports neither body table", path)));
        return false;
    }
    return true;
}

static KwabiStatus
shim_hook_bind_extension(const char *name, const char *path)
{
    int         i;

    if (named_shm == NULL || name == NULL || path == NULL ||
        name[0] == '\0' || strlen(name) >= KWABI_HOOK_NAME_MAX ||
        strlen(path) >= MAXPGPATH)
        return KWABI_ERR_BAD_ARG;
    if (!probe_library(path))
        return KWABI_ERR_BAD_ARG;

    SpinLockAcquire(&named_shm->mutex);
    for (i = 0; i < named_shm->nentries; i++)
        if (strcmp(named_shm->e[i].name, name) == 0)
            break;
    if (i == named_shm->nentries)
    {
        if (named_shm->nentries >= KWABI_HOOK_NAMED_MAX)
        {
            SpinLockRelease(&named_shm->mutex);
            return KWABI_ERR_BAD_ARG;
        }
        strlcpy(named_shm->e[i].name, name, KWABI_HOOK_NAME_MAX);
        named_shm->e[i].generation = 0;
        named_shm->nentries++;
    }
    strlcpy(named_shm->e[i].path, path, MAXPGPATH);
    named_shm->e[i].generation++;
    SpinLockRelease(&named_shm->mutex);

    return KWABI_OK;
}

/* ---- Preload: the name table needs shared memory, reserved before fork. ---- */

static void
kwabi_hook_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
    RequestAddinShmemSpace(MAXALIGN(sizeof(NamedShm)));
}

static void
kwabi_hook_shmem_startup(void)
{
    bool        found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();
    named_shm = ShmemInitStruct("kwabi_hook_named", sizeof(NamedShm), &found);
    if (!found)
    {
        SpinLockInit(&named_shm->mutex);
        named_shm->nentries = 0;
    }
}

/* Called from _PG_init. Only a preloaded runtime gets the table; a plain LOAD leaves
 * named_shm NULL and the reload capability bit clear. */
void
shim_hook_install_reload(void)
{
    if (!process_shared_preload_libraries_in_progress)
        return;
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = kwabi_hook_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = kwabi_hook_shmem_startup;

    /*
     * Every point's trampoline is installed now, not when a body arrives. A backend
     * that never registered a body would otherwise never run the sync, and so would
     * never see a bind made by another backend. With no body in a position the chain
     * is just the standard function, so an idle point costs one sync per call.
     */
    for (int p = 0; p < HOOK_NPOINTS; p++)
        install_point((HookPoint) p);
}

bool
shim_hook_reload_available(void)
{
    return named_shm != NULL;
}

/* ========================================================================
 * The trampolines PostgreSQL calls. No PG_TRY, and no body is called under one.
 * A failed chain is raised here, after it has returned.
 * ======================================================================== */

static void
trampoline_start(QueryDesc *qd, int eflags)
{
    sync_named();
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_start(0, qd, eflags, &err) != KWABI_OK)
        raise_from_error(&err);
}

#if PG_VERSION_NUM >= 180000
static void
trampoline_run(QueryDesc *qd, ScanDirection direction, uint64 count)
{
    sync_named();
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_run(0, qd, (int) direction, count, true, &err) != KWABI_OK)
        raise_from_error(&err);
}
#else
static void
trampoline_run(QueryDesc *qd, ScanDirection direction, uint64 count, bool execute_once)
{
    sync_named();
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_run(0, qd, (int) direction, count, execute_once, &err) != KWABI_OK)
        raise_from_error(&err);
}
#endif

static void
trampoline_finish(QueryDesc *qd)
{
    sync_named();
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_finish(0, qd, &err) != KWABI_OK)
        raise_from_error(&err);
}

static void
trampoline_end(QueryDesc *qd)
{
    sync_named();
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_end(0, qd, &err) != KWABI_OK)
        raise_from_error(&err);
}

static void
trampoline_utility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
                   ProcessUtilityContext context, ParamListInfo params,
                   QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc)
{
    sync_named();
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_utility(0, pstmt, queryString, readOnlyTree, context, params,
                      queryEnv, dest, qc, &err) != KWABI_OK)
        raise_from_error(&err);
}

static bool
trampoline_check_perms(List *rangeTable, List *rtePermInfos, bool ereport_on_violation)
{
    sync_named();
    KwabiError  err;
    int         allowed = 1;

    kwabi_error_init(&err);
    if (chain_check_perms(0, rangeTable, rtePermInfos, ereport_on_violation,
                          &allowed, &err) != KWABI_OK)
        raise_from_error(&err);

    if (!allowed)
    {
        /* A silent denial is an error when the caller asked for one. */
        if (ereport_on_violation)
            ereport(ERROR,
                    (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                     errmsg("permission denied (kwabi hook)")));
        return false;
    }
    return true;
}

static PlannedStmt *
trampoline_planner(Query *parse, const char *query_string, int cursorOptions,
                   ParamListInfo boundParams)
{
    sync_named();
    KwabiError  err;
    PlannedStmt *planned = NULL;

    kwabi_error_init(&err);
    if (chain_planner(0, parse, query_string, cursorOptions, boundParams,
                      &planned, &err) != KWABI_OK)
        raise_from_error(&err);

    if (planned == NULL)
        ereport(ERROR, (errmsg("kwabi: planner hook returned no plan")));
    return planned;
}

/* ========================================================================
 * Registration. The first body on a point installs its trampoline and saves the
 * hook that was there before, which the standard link calls.
 * ======================================================================== */

/* Install this point's trampoline once, saving the hook that was there before. */
static void
install_point(HookPoint point)
{
    if (installed[point])
        return;
    switch (point)
    {
        case HOOK_START:
            prev_start = ExecutorStart_hook;
            ExecutorStart_hook = trampoline_start;
            break;
        case HOOK_RUN:
            prev_run = ExecutorRun_hook;
            ExecutorRun_hook = trampoline_run;
            break;
        case HOOK_FINISH:
            prev_finish = ExecutorFinish_hook;
            ExecutorFinish_hook = trampoline_finish;
            break;
        case HOOK_END:
            prev_end = ExecutorEnd_hook;
            ExecutorEnd_hook = trampoline_end;
            break;
        case HOOK_CHECK_PERMS:
            prev_check_perms = ExecutorCheckPerms_hook;
            ExecutorCheckPerms_hook = trampoline_check_perms;
            break;
        case HOOK_PLANNER:
            prev_planner = planner_hook;
            planner_hook = trampoline_planner;
            break;
        case HOOK_UTILITY:
            prev_utility = ProcessUtility_hook;
            ProcessUtility_hook = trampoline_utility;
            break;
        default:
            break;
    }
    installed[point] = true;
}

static KwabiStatus
register_body(HookPoint point, void *fn, void *arg)
{
    if (fn == NULL || nbodies[point] >= KWABI_HOOK_NAMED_MAX + KWABI_HOOK_MAX_BODIES)
        return KWABI_ERR_BAD_ARG;

    bodies[point][nbodies[point]].fn = fn;
    bodies[point][nbodies[point]].arg = arg;
    nbodies[point]++;

    install_point(point);
    return KWABI_OK;
}

static KwabiStatus
shim_hook_register_executor_start(KwabiExecutorStartBody body, void *arg)
{
    return register_body(HOOK_START, (void *) body, arg);
}

static KwabiStatus
shim_hook_register_executor_run(KwabiExecutorRunBody body, void *arg)
{
    return register_body(HOOK_RUN, (void *) body, arg);
}

static KwabiStatus
shim_hook_register_executor_finish(KwabiExecutorFinishBody body, void *arg)
{
    return register_body(HOOK_FINISH, (void *) body, arg);
}

static KwabiStatus
shim_hook_register_executor_end(KwabiExecutorEndBody body, void *arg)
{
    return register_body(HOOK_END, (void *) body, arg);
}

static KwabiStatus
shim_hook_register_executor_check_perms(KwabiExecutorCheckPermsBody body, void *arg)
{
    return register_body(HOOK_CHECK_PERMS, (void *) body, arg);
}

static KwabiStatus
shim_hook_register_planner(KwabiPlannerBody body, void *arg)
{
    return register_body(HOOK_PLANNER, (void *) body, arg);
}

static KwabiStatus
shim_hook_register_process_utility(KwabiProcessUtilityBody body, void *arg)
{
    return register_body(HOOK_UTILITY, (void *) body, arg);
}

/* ========================================================================
 * `next` slots. A body that was not given a `next` (NULL) cannot continue the
 * chain, and that is reported, not crashed on.
 * ======================================================================== */

static KwabiStatus
next_invalid(KwabiError *err)
{
    if (err != NULL)
        kwabi_error_set_core(err, 0, KWABI_ERR_BAD_ARG,
                             "kwabi: hook_next called with a NULL next handle");
    return KWABI_ERR_BAD_ARG;
}

static KwabiStatus
shim_hook_next_executor_start(KwabiHookNext next, KwabiQueryDesc qd, int eflags,
                              KwabiError *err)
{
    HookLink   *link = (HookLink *) next;

    if (link == NULL)
        return next_invalid(err);
    return chain_start(link->index + 1, (QueryDesc *) qd, eflags, err);
}

static KwabiStatus
shim_hook_next_executor_run(KwabiHookNext next, KwabiQueryDesc qd, int direction,
                            uint64_t count, KwabiError *err)
{
    HookLink   *link = (HookLink *) next;

    if (link == NULL)
        return next_invalid(err);
    return chain_run(link->index + 1, (QueryDesc *) qd, direction, count, link->once, err);
}

static KwabiStatus
shim_hook_next_executor_finish(KwabiHookNext next, KwabiQueryDesc qd, KwabiError *err)
{
    HookLink   *link = (HookLink *) next;

    if (link == NULL)
        return next_invalid(err);
    return chain_finish(link->index + 1, (QueryDesc *) qd, err);
}

static KwabiStatus
shim_hook_next_executor_end(KwabiHookNext next, KwabiQueryDesc qd, KwabiError *err)
{
    HookLink   *link = (HookLink *) next;

    if (link == NULL)
        return next_invalid(err);
    return chain_end(link->index + 1, (QueryDesc *) qd, err);
}

static KwabiStatus
shim_hook_next_executor_check_perms(KwabiHookNext next, KwabiList rangeTable,
                                    KwabiList rtePermInfos, int ereport_on_violation,
                                    int *allowed, KwabiError *err)
{
    HookLink   *link = (HookLink *) next;

    if (link == NULL)
        return next_invalid(err);
    return chain_check_perms(link->index + 1, (List *) rangeTable, (List *) rtePermInfos,
                             ereport_on_violation, allowed, err);
}

static KwabiStatus
shim_hook_next_planner(KwabiHookNext next, KwabiNode parse, const char *queryString,
                       int cursorOptions, KwabiParamListInfo boundParams,
                       KwabiNode *planned, KwabiError *err)
{
    HookLink   *link = (HookLink *) next;

    if (link == NULL)
        return next_invalid(err);
    return chain_planner(link->index + 1, (Query *) parse, queryString, cursorOptions,
                         (ParamListInfo) boundParams, (PlannedStmt **) planned, err);
}

static KwabiStatus
shim_hook_next_process_utility(KwabiHookNext next, KwabiNode pstmt, const char *queryString,
                               int readOnlyTree, int context, KwabiParamListInfo params,
                               KwabiQueryEnvironment queryEnv, KwabiDestReceiver dest,
                               KwabiQueryCompletion qc, KwabiError *err)
{
    HookLink   *link = (HookLink *) next;

    if (link == NULL)
        return next_invalid(err);
    return chain_utility(link->index + 1, (PlannedStmt *) pstmt, queryString,
                         readOnlyTree != 0, (ProcessUtilityContext) context,
                         (ParamListInfo) params, (QueryEnvironment *) queryEnv,
                         (DestReceiver *) dest, (QueryCompletion *) qc, err);
}

void
init_group_hook(void)
{
    /* Unnamed registrations start after the named positions. */
    for (int p = 0; p < HOOK_NPOINTS; p++)
        nbodies[p] = KWABI_HOOK_NAMED_MAX;
    shim_table.hook_bind_extension = shim_hook_bind_extension;
    shim_table.hook_register_process_utility = shim_hook_register_process_utility;
    shim_table.hook_next_process_utility     = shim_hook_next_process_utility;
    shim_table.hook_register_executor_check_perms = shim_hook_register_executor_check_perms;
    shim_table.hook_register_planner              = shim_hook_register_planner;
    shim_table.hook_next_executor_check_perms     = shim_hook_next_executor_check_perms;
    shim_table.hook_next_planner                  = shim_hook_next_planner;
    shim_table.hook_register_executor_start  = shim_hook_register_executor_start;
    shim_table.hook_register_executor_run    = shim_hook_register_executor_run;
    shim_table.hook_register_executor_finish = shim_hook_register_executor_finish;
    shim_table.hook_register_executor_end    = shim_hook_register_executor_end;
    shim_table.hook_next_executor_start      = shim_hook_next_executor_start;
    shim_table.hook_next_executor_run        = shim_hook_next_executor_run;
    shim_table.hook_next_executor_finish     = shim_hook_next_executor_finish;
    shim_table.hook_next_executor_end        = shim_hook_next_executor_end;
}

/* ========================================================================
 * Test support. The bodies below are registered through the ABI table, the way an
 * extension would register them. They record a trace for statements whose text
 * contains "kwt_trace", so the test's own queries do not write to it.
 * ======================================================================== */

#define KWABI_HOOK_TEST_TRACE_MAX 64

static char test_trace[KWABI_HOOK_TEST_TRACE_MAX];
static int  test_tracelen = 0;
static bool test_installed = false;
static bool test_refuse = false;

static bool
test_traced(QueryDesc *qd)
{
    return qd != NULL && qd->sourceText != NULL &&
        strstr(qd->sourceText, "kwt_trace") != NULL;
}

static void
test_record(const char *tag, bool traced)
{
    if (!traced || test_tracelen >= KWABI_HOOK_TEST_TRACE_MAX - 1)
        return;
    test_trace[test_tracelen++] = tag[0];
    test_trace[test_tracelen] = '\0';
}

static KwabiStatus
test_start(KwabiQueryDesc qd, int eflags, KwabiHookNext next, KwabiError *err, void *arg)
{
    test_record((const char *) arg, test_traced((QueryDesc *) qd));
    return shim_api->hook_next_executor_start(next, qd, eflags, err);
}

static KwabiStatus
test_run(KwabiQueryDesc qd, int direction, uint64_t count, KwabiHookNext next,
         KwabiError *err, void *arg)
{
    bool        traced = test_traced((QueryDesc *) qd);

    test_record((const char *) arg, traced);
    if (traced && test_refuse && strcmp((const char *) arg, "2") == 0)
    {
        kwabi_error_init(err);
        kwabi_error_set_core(err, ERRCODE_RAISE_EXCEPTION, KWABI_ERR_BODY_RAISED,
                             "kwabi hook refused the statement");
        return KWABI_ERR_BODY_RAISED;
    }
    return shim_api->hook_next_executor_run(next, qd, direction, count, err);
}

static KwabiStatus
test_finish(KwabiQueryDesc qd, KwabiHookNext next, KwabiError *err, void *arg)
{
    test_record((const char *) arg, test_traced((QueryDesc *) qd));
    return shim_api->hook_next_executor_finish(next, qd, err);
}

static KwabiStatus
test_end(KwabiQueryDesc qd, KwabiHookNext next, KwabiError *err, void *arg)
{
    test_record((const char *) arg, test_traced((QueryDesc *) qd));
    return shim_api->hook_next_executor_end(next, qd, err);
}

/* Registration is per backend and cannot be undone, so it happens once. */
PG_FUNCTION_INFO_V1(kwabi_hook_test_install);
Datum
kwabi_hook_test_install(PG_FUNCTION_ARGS)
{
    if (test_installed)
        PG_RETURN_TEXT_P(cstring_to_text("already installed"));

    if (shim_api == NULL ||
        shim_api->hook_register_executor_start == NULL ||
        shim_api->hook_register_executor_run == NULL ||
        shim_api->hook_register_executor_finish == NULL ||
        shim_api->hook_register_executor_end == NULL)
        ereport(ERROR, (errmsg("kwabi: hook slots are not wired")));

    /* Two run bodies, so the chain order is observable: "1" outside, "2" inside. */
    if (shim_api->hook_register_executor_start(test_start, (void *) "S") != KWABI_OK ||
        shim_api->hook_register_executor_run(test_run, (void *) "1") != KWABI_OK ||
        shim_api->hook_register_executor_run(test_run, (void *) "2") != KWABI_OK ||
        shim_api->hook_register_executor_finish(test_finish, (void *) "F") != KWABI_OK ||
        shim_api->hook_register_executor_end(test_end, (void *) "E") != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: hook registration failed")));

    test_installed = true;
    PG_RETURN_TEXT_P(cstring_to_text("installed"));
}

PG_FUNCTION_INFO_V1(kwabi_hook_test_clear);
Datum
kwabi_hook_test_clear(PG_FUNCTION_ARGS)
{
    test_tracelen = 0;
    test_trace[0] = '\0';
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(kwabi_hook_test_trace);
Datum
kwabi_hook_test_trace(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text(test_trace));
}

PG_FUNCTION_INFO_V1(kwabi_hook_test_refuse);
Datum
kwabi_hook_test_refuse(PG_FUNCTION_ARGS)
{
    test_refuse = PG_GETARG_BOOL(0);
    PG_RETURN_BOOL(test_refuse);
}

/* Permission-check and planner test bodies. The planner runs first for a statement,
 * so it sets the flag that tells the permission-check body the statement is traced. */
static bool test_planner_traced = false;
static bool test_deny = false;
static bool test_points_installed = false;

static KwabiStatus
test_planner(KwabiNode parse, const char *queryString, int cursorOptions,
             KwabiParamListInfo boundParams, KwabiHookNext next, KwabiNode *planned,
             KwabiError *err, void *arg)
{
    test_planner_traced = queryString != NULL && strstr(queryString, "kwt_trace") != NULL;
    test_record((const char *) arg, test_planner_traced);
    return shim_api->hook_next_planner(next, parse, queryString, cursorOptions,
                                       boundParams, planned, err);
}

static KwabiStatus
test_check_perms(KwabiList rangeTable, KwabiList rtePermInfos, int ereport_on_violation,
                 KwabiHookNext next, int *allowed, KwabiError *err, void *arg)
{
    test_record((const char *) arg, test_planner_traced);
    if (test_planner_traced && test_deny)
    {
        kwabi_error_init(err);
        kwabi_error_set_core(err, ERRCODE_INSUFFICIENT_PRIVILEGE, KWABI_ERR_BODY_RAISED,
                             "denied by kwabi test body");
        return KWABI_ERR_BODY_RAISED;
    }
    return shim_api->hook_next_executor_check_perms(next, rangeTable, rtePermInfos,
                                                    ereport_on_violation, allowed, err);
}

PG_FUNCTION_INFO_V1(kwabi_hook_test_install_planner);
Datum
kwabi_hook_test_install_planner(PG_FUNCTION_ARGS)
{
    if (test_points_installed)
        PG_RETURN_TEXT_P(cstring_to_text("already installed"));

    if (shim_api == NULL ||
        shim_api->hook_register_planner == NULL ||
        shim_api->hook_register_executor_check_perms == NULL)
        ereport(ERROR, (errmsg("kwabi: planner and check-perms slots are not wired")));

    if (shim_api->hook_register_planner(test_planner, (void *) "P") != KWABI_OK ||
        shim_api->hook_register_executor_check_perms(test_check_perms, (void *) "C") != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: hook registration failed")));

    test_points_installed = true;
    PG_RETURN_TEXT_P(cstring_to_text("installed"));
}

PG_FUNCTION_INFO_V1(kwabi_hook_test_deny);
Datum
kwabi_hook_test_deny(PG_FUNCTION_ARGS)
{
    test_deny = PG_GETARG_BOOL(0);
    PG_RETURN_BOOL(test_deny);
}

/* Utility test body. Records the traced statements; can deny them. */
static bool test_utility_deny = false;

static KwabiStatus
test_utility(KwabiNode pstmt, const char *queryString, int readOnlyTree, int context,
             KwabiParamListInfo params, KwabiQueryEnvironment queryEnv,
             KwabiDestReceiver dest, KwabiQueryCompletion qc, KwabiHookNext next,
             KwabiError *err, void *arg)
{
    bool        traced = queryString != NULL && strstr(queryString, "kwt_trace") != NULL;

    test_record((const char *) arg, traced);
    if (traced && test_utility_deny)
    {
        kwabi_error_init(err);
        kwabi_error_set_core(err, ERRCODE_INSUFFICIENT_PRIVILEGE, KWABI_ERR_BODY_RAISED,
                             "utility denied by kwabi test body");
        return KWABI_ERR_BODY_RAISED;
    }
    return shim_api->hook_next_process_utility(next, pstmt, queryString, readOnlyTree,
                                               context, params, queryEnv, dest, qc, err);
}

PG_FUNCTION_INFO_V1(kwabi_hook_test_install_utility);
Datum
kwabi_hook_test_install_utility(PG_FUNCTION_ARGS)
{
    static bool installed = false;

    if (installed)
        PG_RETURN_TEXT_P(cstring_to_text("already installed"));
    if (shim_api == NULL || shim_api->hook_register_process_utility == NULL)
        ereport(ERROR, (errmsg("kwabi: utility slot is not wired")));
    if (shim_api->hook_register_process_utility(test_utility, (void *) "U") != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: hook registration failed")));
    installed = true;
    PG_RETURN_TEXT_P(cstring_to_text("installed"));
}

PG_FUNCTION_INFO_V1(kwabi_hook_test_deny_utility);
Datum
kwabi_hook_test_deny_utility(PG_FUNCTION_ARGS)
{
    test_utility_deny = PG_GETARG_BOOL(0);
    PG_RETURN_BOOL(test_utility_deny);
}

/* Test entry point: bind a name to a library, as an extension would. */
PG_FUNCTION_INFO_V1(kwabi_hook_test_bind);
Datum
kwabi_hook_test_bind(PG_FUNCTION_ARGS)
{
    KwabiStatus st = shim_api->hook_bind_extension(text_to_cstring(PG_GETARG_TEXT_PP(0)),
                                                   text_to_cstring(PG_GETARG_TEXT_PP(1)));
    PG_RETURN_TEXT_P(cstring_to_text(st == KWABI_OK ? "bound" : "bind refused"));
}

/*
 * Type I/O lookup for the SQL functions in group_type_io.c. Returns false when the name
 * is not bound, or is bound to a library with no type table.
 */
bool
shim_type_bodies_lookup(const char *name, KwabiTypeBodies *out)
{
    bool        found = false;

    sync_named();
    if (named_shm == NULL)
        return false;

    SpinLockAcquire(&named_shm->mutex);
    for (int i = 0; i < named_shm->nentries; i++)
    {
        if (strcmp(named_shm->e[i].name, name) == 0)
        {
            if (named_types[i].input != NULL)
            {
                *out = named_types[i];
                found = true;
            }
            break;
        }
    }
    SpinLockRelease(&named_shm->mutex);
    return found;
}

/* Raise a KwabiError from a SQL function. */
void
shim_raise_kwabi_error(const KwabiError *err)
{
    raise_from_error(err);
}

/*
 * Aggregate table lookup for the SQL functions in group_aggregate.c. Returns false when
 * the name is not bound, or is bound to a library with no aggregate table.
 */
bool
shim_agg_bodies_lookup(const char *name, KwabiAggBodies *out)
{
    bool        found = false;

    sync_named();
    if (named_shm == NULL)
        return false;

    SpinLockAcquire(&named_shm->mutex);
    for (int i = 0; i < named_shm->nentries; i++)
    {
        if (strcmp(named_shm->e[i].name, name) == 0)
        {
            if (named_aggs[i].init != NULL)
            {
                *out = named_aggs[i];
                found = true;
            }
            break;
        }
    }
    SpinLockRelease(&named_shm->mutex);
    return found;
}
