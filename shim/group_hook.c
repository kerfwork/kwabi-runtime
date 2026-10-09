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
#include "utils/builtins.h"

#define KWABI_HOOK_MAX_BODIES 8

typedef enum HookPoint
{
    HOOK_START = 0,
    HOOK_RUN,
    HOOK_FINISH,
    HOOK_END,
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

static HookBody bodies[HOOK_NPOINTS][KWABI_HOOK_MAX_BODIES];
static int  nbodies[HOOK_NPOINTS];
static bool installed[HOOK_NPOINTS];

/* The hook each point had before we installed ours. Null means none. */
static ExecutorStart_hook_type prev_start = NULL;
static ExecutorRun_hook_type prev_run = NULL;
static ExecutorFinish_hook_type prev_finish = NULL;
static ExecutorEnd_hook_type prev_end = NULL;

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

/* ========================================================================
 * Chains. chain_X(index, ...) runs body `index`, or the standard link once the
 * registered bodies are exhausted. The `next` handle a body receives is a HookLink
 * that names the body it was given to; hook_next_X continues from index + 1.
 * ======================================================================== */

static KwabiStatus
chain_start(int index, QueryDesc *qd, int eflags, KwabiError *err)
{
    if (index < nbodies[HOOK_START])
    {
        HookLink    link = {index, false};
        HookBody   *b = &bodies[HOOK_START][index];

        return ((KwabiExecutorStartBody) b->fn)((KwabiQueryDesc) qd, eflags,
                                                (KwabiHookNext) &link, err, b->arg);
    }
    return standard_start(qd, eflags, err);
}

static KwabiStatus
chain_run(int index, QueryDesc *qd, int direction, uint64_t count, bool once, KwabiError *err)
{
    if (index < nbodies[HOOK_RUN])
    {
        HookLink    link = {index, once};
        HookBody   *b = &bodies[HOOK_RUN][index];

        return ((KwabiExecutorRunBody) b->fn)((KwabiQueryDesc) qd, direction, count,
                                              (KwabiHookNext) &link, err, b->arg);
    }
    return standard_run(qd, direction, count, once, err);
}

static KwabiStatus
chain_finish(int index, QueryDesc *qd, KwabiError *err)
{
    if (index < nbodies[HOOK_FINISH])
    {
        HookLink    link = {index, false};
        HookBody   *b = &bodies[HOOK_FINISH][index];

        return ((KwabiExecutorFinishBody) b->fn)((KwabiQueryDesc) qd,
                                                 (KwabiHookNext) &link, err, b->arg);
    }
    return standard_finish(qd, err);
}

static KwabiStatus
chain_end(int index, QueryDesc *qd, KwabiError *err)
{
    if (index < nbodies[HOOK_END])
    {
        HookLink    link = {index, false};
        HookBody   *b = &bodies[HOOK_END][index];

        return ((KwabiExecutorEndBody) b->fn)((KwabiQueryDesc) qd,
                                              (KwabiHookNext) &link, err, b->arg);
    }
    return standard_end(qd, err);
}

/* ========================================================================
 * The trampolines PostgreSQL calls. No PG_TRY, and no body is called under one.
 * A failed chain is raised here, after it has returned.
 * ======================================================================== */

static void
trampoline_start(QueryDesc *qd, int eflags)
{
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_start(0, qd, eflags, &err) != KWABI_OK)
        raise_from_error(&err);
}

#if PG_VERSION_NUM >= 180000
static void
trampoline_run(QueryDesc *qd, ScanDirection direction, uint64 count)
{
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_run(0, qd, (int) direction, count, true, &err) != KWABI_OK)
        raise_from_error(&err);
}
#else
static void
trampoline_run(QueryDesc *qd, ScanDirection direction, uint64 count, bool execute_once)
{
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_run(0, qd, (int) direction, count, execute_once, &err) != KWABI_OK)
        raise_from_error(&err);
}
#endif

static void
trampoline_finish(QueryDesc *qd)
{
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_finish(0, qd, &err) != KWABI_OK)
        raise_from_error(&err);
}

static void
trampoline_end(QueryDesc *qd)
{
    KwabiError  err;

    kwabi_error_init(&err);
    if (chain_end(0, qd, &err) != KWABI_OK)
        raise_from_error(&err);
}

/* ========================================================================
 * Registration. The first body on a point installs its trampoline and saves the
 * hook that was there before, which the standard link calls.
 * ======================================================================== */

static KwabiStatus
register_body(HookPoint point, void *fn, void *arg)
{
    if (fn == NULL || nbodies[point] >= KWABI_HOOK_MAX_BODIES)
        return KWABI_ERR_BAD_ARG;

    bodies[point][nbodies[point]].fn = fn;
    bodies[point][nbodies[point]].arg = arg;
    nbodies[point]++;

    if (!installed[point])
    {
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
            default:
                break;
        }
        installed[point] = true;
    }
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

void
init_group_hook(void)
{
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
