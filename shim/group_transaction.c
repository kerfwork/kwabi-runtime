/* group_transaction.c — transaction proof functions for the kwabi shim */

#include "shim_internal.h"

/* ========================================================================
 * Transaction shim functions
 * ======================================================================== */

void
shim_transaction_start(void)
{
    StartTransactionCommand();
}

void
shim_transaction_commit(void)
{
    CommitTransactionCommand();
}

void
shim_transaction_abort(void)
{
    AbortCurrentTransaction();
}

bool
shim_transaction_is_active(void)
{
    return IsTransactionBlock();
}

int64
shim_transaction_get_current_xid(void)
{
    return (int64) GetCurrentTransactionId();
}

void
init_group_transaction(void)
{
    shim_table.transaction_start = shim_transaction_start;
    shim_table.transaction_commit = shim_transaction_commit;
    shim_table.transaction_abort = shim_transaction_abort;
    shim_table.transaction_is_active = shim_transaction_is_active;
    shim_table.transaction_get_current_xid = shim_transaction_get_current_xid;
}

/* ========================================================================
 * Transaction proof functions
 * ======================================================================== */

/*
 * kwabi_transaction_test() -> text
 *
 * Test transaction operations through the ABI.
 * Starts a transaction, checks is_active, gets the xid, commits, and
 * verifies the transaction is no longer active.
 */
PG_FUNCTION_INFO_V1(kwabi_transaction_test);

Datum
kwabi_transaction_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->transaction_start == NULL || api->transaction_commit == NULL ||
        api->transaction_abort == NULL || api->transaction_is_active == NULL ||
        api->transaction_get_current_xid == NULL)
        ereport(ERROR, (errmsg("kwabi: transaction slots are not wired")));

    /* Start a transaction */
    api->transaction_start();

    /* Must be active */
    if (!api->transaction_is_active())
        ereport(ERROR, (errmsg("kwabi: transaction_is_active returned false after start")));

    /* Get the current xid */
    int64 xid = api->transaction_get_current_xid();
    if (xid == 0)
        ereport(ERROR, (errmsg("kwabi: transaction_get_current_xid returned 0")));

    /* Commit */
    api->transaction_commit();

    /* Must NOT be active */
    if (api->transaction_is_active())
        ereport(ERROR, (errmsg("kwabi: transaction_is_active returned true after commit")));

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("transaction_test: xid=%ld", (long) xid)));
}

/*
 * kwabi_transaction_abort_test() -> bool
 *
 * Test transaction abort through the ABI.
 * Starts a transaction, aborts it, and verifies it is no longer active.
 */
PG_FUNCTION_INFO_V1(kwabi_transaction_abort_test);

Datum
kwabi_transaction_abort_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->transaction_start == NULL || api->transaction_abort == NULL ||
        api->transaction_is_active == NULL)
        ereport(ERROR, (errmsg("kwabi: transaction slots are not wired")));

    /* Start a transaction */
    api->transaction_start();

    /* Must be active */
    if (!api->transaction_is_active())
        ereport(ERROR, (errmsg("kwabi: transaction_is_active returned false after start")));

    /* Abort */
    api->transaction_abort();

    /* Must NOT be active */
    if (api->transaction_is_active())
        ereport(ERROR, (errmsg("kwabi: transaction_is_active returned true after abort")));

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_transaction_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts a wrong xid, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_transaction_control);

Datum
kwabi_transaction_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->transaction_start == NULL ||
        shim_api->transaction_is_active == NULL ||
        shim_api->transaction_get_current_xid == NULL ||
        shim_api->transaction_commit == NULL)
        ereport(ERROR, (errmsg("kwabi: transaction slots are not wired")));

    shim_api->transaction_start();

    int64 xid = shim_api->transaction_get_current_xid();

    shim_api->transaction_commit();

    /* The control's whole point: this comparison must be FALSE. */
    if (xid == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks xid is 999: broken */

    ereport(ERROR,
            (errmsg("kwabi: transaction negative control fired as intended"),
             errdetail("xid is not 999 -- the value comparison is honest")));
}

