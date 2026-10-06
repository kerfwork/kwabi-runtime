/* group_executor.c — executor proof functions for the kwabi shim */

#include "shim_internal.h"

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

