/*
 * fwprobe2.c — the decisive error-firewall probes.
 *
 * fwprobe.c asked "does the backend survive a caught ERROR?" and the answer
 * was yes, even without a subtransaction. That was a weak question. The real
 * one is:
 *
 *   If an ERROR is caught and swallowed, does the work already done in that
 *   scope survive?
 *
 * It must not. An extension that catches an error and continues is asserting
 * "that attempt had no effect". If partial writes survive, the claim is false
 * and the database is silently inconsistent — the worst kind of failure.
 *
 * Measurement notes (both were wrong in the first version of this file, and
 * both produced a false positive):
 *
 *  - SPI_execute with read_only=true does NOT see the caller's own uncommitted
 *    writes, because it skips CommandCounterIncrement. Counting with it
 *    reported 0 rows that were in fact there. Counts below use read_only=false.
 *  - Each insert is tagged with its source, so a row left behind by an earlier
 *    function cannot be mistaken for one left behind by the function under
 *    test. The first version conflated the two.
 */

#include "postgres.h"

#include "fmgr.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/* PG_MODULE_MAGIC lives in fwprobe.c: one magic block per module. */

PG_FUNCTION_INFO_V1(fw_partial_no_subxact);
PG_FUNCTION_INFO_V1(fw_partial_with_subxact);

/*
 * Count rows carrying `tag`.
 *
 * read_only=false so the command counter advances and the caller's own insert
 * is visible. read_only=true would report 0 and make a surviving write look
 * rolled back.
 */
static int
count_tagged(int tag)
{
    int   ret;
    bool  isnull;
    Datum d;
    char  sql[64];

    snprintf(sql, sizeof(sql), "SELECT count(*) FROM fw_t WHERE tag = %d", tag);

    ret = SPI_execute(sql, false, 0);
    if (ret != SPI_OK_SELECT || SPI_processed == 0)
        return -1;

    d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
        return -1;
    return DatumGetInt32(d);
}

static int
insert_tagged(int tag)
{
    char sql[64];
    snprintf(sql, sizeof(sql), "INSERT INTO fw_t VALUES (%d)", tag);
    return SPI_execute(sql, false, 0);
}

/*
 * fw_partial_no_subxact() — insert, then raise and swallow with PG_TRY alone.
 *
 * Reports how many rows *it* inserted are still visible. A non-zero result
 * means the swallowed error left a write behind that the caller believes did
 * not happen.
 */
Datum
fw_partial_no_subxact(PG_FUNCTION_ARGS)
{
    int  tag = PG_GETARG_INT32(0);
    int  after = -1;
    bool caught = false;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "fwprobe: SPI_connect failed");

    if (insert_tagged(tag) != SPI_OK_INSERT)
        elog(ERROR, "fwprobe: insert failed");

    PG_TRY();
    {
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("fwprobe: deliberate error")));
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        FlushErrorState();
        FreeErrorData(edata);
        caught = true;
    }
    PG_END_TRY();

    after = count_tagged(tag);

    SPI_finish();

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("no_subxact(tag=%d): caught=%s own_rows_survived=%d%s",
                 tag, caught ? "yes" : "no", after,
                 after > 0 ? "  <-- PARTIAL WORK SURVIVED" : "  (rolled back)")));
}

/*
 * fw_partial_with_subxact() — the same insert and error, but the insert is
 * inside an internal subtransaction that is rolled back on catch.
 */
Datum
fw_partial_with_subxact(PG_FUNCTION_ARGS)
{
    int  tag = PG_GETARG_INT32(0);
    int  after = -1;
    bool caught = false;

    MemoryContext oldcontext = CurrentMemoryContext;
    ResourceOwner oldowner = CurrentResourceOwner;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "fwprobe: SPI_connect failed");

    BeginInternalSubTransaction(NULL);

    PG_TRY();
    {
        if (insert_tagged(tag) != SPI_OK_INSERT)
            elog(ERROR, "fwprobe: insert failed");

        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("fwprobe: deliberate error")));
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        FlushErrorState();
        FreeErrorData(edata);
        caught = true;

        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(oldcontext);
    CurrentResourceOwner = oldowner;

    after = count_tagged(tag);

    SPI_finish();

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("with_subxact(tag=%d): caught=%s own_rows_survived=%d%s",
                 tag, caught ? "yes" : "no", after,
                 after > 0 ? "  <-- PARTIAL WORK SURVIVED" : "  (rolled back)")));
}
