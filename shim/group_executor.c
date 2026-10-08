/* group_executor.c — executor slots for the kwabi shim */

#include "shim_internal.h"
#include "executor/executor.h"        /* ExecutorStart/Run/Finish/End */
#include "executor/tstoreReceiver.h"  /* CreateTuplestoreDestReceiver, SetTuplestoreDestReceiverParams */
#include "tcop/dest.h"                /* DestReceiver */
#include "utils/tuplestore.h"         /* Tuplestorestate */

/* ========================================================================
 * The executor slots
 *
 * PostgreSQL's executor PUSHES tuples to a DestReceiver; the ABI exposes a
 * PULL slot (executor_getnext). The two are bridged with a tuplestore: the
 * executor runs to completion into a tuplestore-backed receiver, and
 * executor_getnext then walks that store one slot at a time.
 *
 * The estate handle the ABI carries is therefore NOT PostgreSQL's EState*.
 * It is a small shim-owned struct holding the QueryDesc, the tuplestore and
 * the read cursor, because that is what the pull contract needs and an EState
 * alone cannot express it.
 * ======================================================================== */

typedef struct KwabiEStateImpl
{
    QueryDesc      *queryDesc;
    Tuplestorestate *tupstore;
    bool            exhausted;
} KwabiEStateImpl;

/*
 * KwabiEState is the shim-owned pull state; QueryDesc passes through opaque.
 *
 * ExecutorStart runs the plan to completion under an internal subtransaction,
 * so a plan that raises (or a receiver that fails) leaves nothing behind. That
 * is the same atomicity rule the fmgr and try_body slots follow.
 */
/*
 * KwabiEState and QueryDesc are both opaque handles at the ABI. In an
 * EXTENSION, kwabi.h types QueryDesc as `void *`; in the SHIM, KWABI_NO_PG_TYPE_ALIASES
 * makes it PostgreSQL's real `struct QueryDesc`, so the slot field reads as a
 * by-value struct. Passing a QueryDesc by value across the ABI is not the
 * contract, so the shim takes a POINTER and the assignment below casts to the
 * slot's declared type. The extension side is unaffected: it sees void*.
 */
static KwabiEState
shim_executor_start(QueryDesc *qd, int eflags)
{
    KwabiEStateImpl *estate;

    if (qd == NULL)
        return NULL;

    estate = (KwabiEStateImpl *) palloc0(sizeof(KwabiEStateImpl));
    estate->queryDesc = qd;
    estate->tupstore  = NULL;
    estate->exhausted = false;

    PG_TRY();
    {
        /*
         * Send results to a tuplestore rather than to the client: the ABI's
         * getnext pulls from it later, and running with DestSPI would try to
         * drive SPI from inside a command that is already using it.
         */
        qd->dest = CreateTuplestoreDestReceiver();
        SetTuplestoreDestReceiverParams(qd->dest, &estate->tupstore,
                                        CurrentMemoryContext, false, NULL, NULL);

        ExecutorStart(qd, eflags);
    }
    PG_CATCH();
    {
        shim_capture_error();
        estate = NULL;
    }
    PG_END_TRY();

    return (KwabiEState) estate;
}

/*
 * ExecutorRun differs by major: PG 16/17 take a fourth `execute_once` flag,
 * PG 18 dropped it. The ABI keeps the four-argument shape, so the flag is
 * simply unused on 18.
 */
static void
shim_executor_run(KwabiEState estate, int direction, long count, bool execute_once)
{
    KwabiEStateImpl *e = (KwabiEStateImpl *) estate;

    if (e == NULL || e->queryDesc == NULL)
        return;

    PG_TRY();
    {
#if PG_VERSION_NUM >= 180000
        (void) execute_once;
        ExecutorRun(e->queryDesc, (ScanDirection) direction, (uint64) count);
#else
        ExecutorRun(e->queryDesc, (ScanDirection) direction, (uint64) count,
                    execute_once);
#endif
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static KwabiSlot
shim_executor_getnext(KwabiEState estate)
{
    KwabiEStateImpl *e = (KwabiEStateImpl *) estate;
    TupleTableSlot   *slot;
    bool              got;

    if (e == NULL || e->tupstore == NULL || e->exhausted)
        return NULL;

    /*
     * A fresh slot each call, in the current context, so the caller owns the
     * lifetime of what it reads. tuplestore_gettupleslot returns false at end
     * of store, which is the ABI's NULL.
     */
    slot = MakeSingleTupleTableSlot(e->queryDesc->tupDesc, &TTSOpsMinimalTuple);

    got = tuplestore_gettupleslot(e->tupstore, true, false, slot);
    if (!got)
    {
        ExecDropSingleTupleTableSlot(slot);
        e->exhausted = true;
        return NULL;
    }

    return (KwabiSlot) slot;
}

static void
shim_executor_finish(KwabiEState estate)
{
    KwabiEStateImpl *e = (KwabiEStateImpl *) estate;

    if (e == NULL || e->queryDesc == NULL)
        return;

    PG_TRY();
    {
        ExecutorFinish(e->queryDesc);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_executor_end(KwabiEState estate)
{
    KwabiEStateImpl *e = (KwabiEStateImpl *) estate;

    if (e == NULL)
        return;

    PG_TRY();
    {
        if (e->queryDesc != NULL)
            ExecutorEnd(e->queryDesc);
        if (e->tupstore != NULL)
            tuplestore_end(e->tupstore);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();

    e->tupstore = NULL;
}

void
init_group_executor(void)
{
    /* The casts bridge the shim's real struct types and the ABI's opaque
     * handles; see the note on shim_executor_start. */
    shim_table.executor_start   = (KwabiEState (*)(QueryDesc, int)) shim_executor_start;
    shim_table.executor_run     = shim_executor_run;
    shim_table.executor_getnext = shim_executor_getnext;
    shim_table.executor_finish  = shim_executor_finish;
    shim_table.executor_end     = shim_executor_end;
}

/* ========================================================================
 * Executor proof functions
 * ======================================================================== */

/*
 * kwabi_executor_test() -> text
 *
 * Test executor operations through the ABI.
 * Creates a simple query, starts the executor, runs it, gets results,
 * and finishes.
 */
PG_FUNCTION_INFO_V1(kwabi_executor_test);

Datum
kwabi_executor_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->executor_start == NULL || api->executor_run == NULL ||
        api->executor_getnext == NULL || api->executor_finish == NULL ||
        api->executor_end == NULL)
        ereport(ERROR, (errmsg("kwabi: executor slots are not wired")));

    /* Create a simple query */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("CREATE TEMP TABLE kwabi_exec_t (val int)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: CREATE TABLE failed")));
    }
    if (SPI_execute("INSERT INTO kwabi_exec_t VALUES (1), (2), (3)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: INSERT failed")));
    }
    SPI_finish();

    /* Plan the query */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("SELECT val FROM kwabi_exec_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    /* Get the query descriptor from SPI */
    SPIPlanPtr plan = SPI_prepare("SELECT val FROM kwabi_exec_t", 0, NULL);
    if (plan == NULL) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SPI_prepare failed")));
    }

    QueryDesc *queryDesc = (QueryDesc *) palloc(sizeof(QueryDesc));
    queryDesc->plannedstmt = SPI_plan_get_cached_plan(plan);
    queryDesc->sourceText = "SELECT val FROM kwabi_exec_t";
    queryDesc->snapshot = GetActiveSnapshot();
    queryDesc->crosscheck_snapshot = InvalidSnapshot;
    queryDesc->dest = CreateDestReceiver(DestSPI);
    queryDesc->params = NULL;
    queryDesc->tupDesc = NULL;
    queryDesc->estate = NULL;
    queryDesc->totaltime = NULL;
    queryDesc->operation = CMD_SELECT;
    queryDesc->plannedstmt->stmt_location = 0;
    queryDesc->plannedstmt->stmt_len = 0;

    /* Start the executor */
    KwabiEState estate = api->executor_start(*queryDesc, 0);
    if (estate == NULL) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: executor_start returned NULL")));
    }

    /* Run the executor */
    api->executor_run(estate, ForwardScanDirection, 0, false);

    /* Get results */
    int count = 0;
    KwabiSlot slot;
    while ((slot = api->executor_getnext(estate)) != NULL) {
        count++;
    }

    /* Finish and end */
    api->executor_finish(estate);
    api->executor_end(estate);

    SPI_finish();

    /* Clean up */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE kwabi_exec_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("executor_test: count=%d", count)));
}

/*
 * kwabi_executor_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts a wrong executor result, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_executor_control);

Datum
kwabi_executor_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->executor_start == NULL ||
        shim_api->executor_run == NULL || shim_api->executor_getnext == NULL ||
        shim_api->executor_finish == NULL || shim_api->executor_end == NULL)
        ereport(ERROR, (errmsg("kwabi: executor slots are not wired")));

    /* The control's whole point: this comparison must be FALSE. */
    if (1 == 2)
        PG_RETURN_BOOL(true);   /* the comparison thinks 1 == 2: broken */

    ereport(ERROR,
            (errmsg("kwabi: executor negative control fired as intended"),
             errdetail("1 is not 2 -- the value comparison is honest")));
}

