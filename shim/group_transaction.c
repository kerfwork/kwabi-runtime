/* group_transaction.c — transaction accessor for the kwabi shim */

#include "shim_internal.h"

/* ========================================================================
 * Transaction shim functions
 * ======================================================================== */

/*
 * There is exactly one transaction slot, and this is it.
 *
 * Earlier revisions wired transaction_start/commit/abort/is_active onto
 * PostgreSQL's BLOCK-level API (BeginTransactionBlock / EndTransactionBlock /
 * UserAbortTransactionBlock). That was wrong, and measurably so:
 *
 *   * The command-level API (StartTransactionCommand) is illegal from inside a
 *     SQL-callable function: the function is already running inside a command's
 *     transaction, so it raises "StartTransactionCommand: unexpected state
 *     STARTED".
 *   * The block-level API is the tcop command-loop state machine that the
 *     BEGIN/COMMIT *statements* drive. Driving it from inside a command does
 *     not nest a transaction -- it corrupts the state machine. Observed:
 *     "FATAL: EndTransactionBlock: unexpected state BEGIN", which drops the
 *     connection.
 *
 * PostgreSQL has a single flat transaction per session. Real transaction
 * boundaries inside a routine belong to the PL layer (a procedure's COMMIT,
 * reachable only through CALL), not to a general extension ABI; and
 * partial-rollback atomicity is already the `try_body` slot's job (it runs the
 * body inside PG_TRY and an internal subtransaction). So start/commit/abort are
 * not a surface this ABI can honestly expose, and a NULL slot would be a
 * promise it could never keep. They are removed, not nulled.
 *
 * What remains is the identity accessor: tag extension state with the current
 * transaction.
 */

/*
 * transaction_get_current_xid — the current top-level transaction id, or 0 if
 * the transaction has not been assigned one yet.
 *
 * GetTopTransactionIdIfAny(), deliberately, NOT GetCurrentTransactionId().
 * The latter ASSIGNS an XID if none exists yet -- asking for the id would force
 * one into existence, changing transaction behaviour as a side effect of a
 * read. Measured: after pg_current_xact_id_if_assigned() reports NULL,
 * pg_current_xact_id() returns a fresh XID and if_assigned then reports it.
 * The accessor must be a pure read, so it uses the non-allocating form.
 *
 * 0 is the honest answer for a read-only transaction (no write, hence no XID
 * yet); it is not an error.
 */
int64
shim_transaction_get_current_xid(void)
{
    return (int64) GetTopTransactionIdIfAny();
}

void
init_group_transaction(void)
{
    shim_table.transaction_get_current_xid = shim_transaction_get_current_xid;
}

/* ========================================================================
 * Transaction proof functions
 * ======================================================================== */

/*
 * kwabi_transaction_xid() -> int64
 *
 * Returns the current transaction id straight from the ABI slot, so the SQL
 * harness can assert on it. This is the whole remaining transaction surface.
 */
PG_FUNCTION_INFO_V1(kwabi_transaction_xid);

Datum
kwabi_transaction_xid(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    if (shim_api->transaction_get_current_xid == NULL)
        ereport(ERROR, (errmsg("kwabi: transaction slot is not wired")));

    PG_RETURN_INT64(shim_api->transaction_get_current_xid());
}

/*
 * kwabi_transaction_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts the xid equals a value it cannot be (999),
 * and must therefore RAISE. If it returns a row instead of erroring, the
 * equality check above is vacuous. Same standard as fmgr-api.sql and
 * capabilities-design.md section 5.
 */
PG_FUNCTION_INFO_V1(kwabi_transaction_control);

Datum
kwabi_transaction_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->transaction_get_current_xid == NULL)
        ereport(ERROR, (errmsg("kwabi: transaction slot is not wired")));

    int64 xid = shim_api->transaction_get_current_xid();

    /* The control's whole point: this comparison must be FALSE. */
    if (xid == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks xid is 999: broken */

    ereport(ERROR,
            (errmsg("kwabi: transaction negative control fired as intended"),
             errdetail("xid is not 999 -- the value comparison is honest")));
}
