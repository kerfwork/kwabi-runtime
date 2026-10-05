/*
 * fwprobe.c — empirical probes for the error-firewall design.
 *
 * These answer, by measurement rather than assertion, the two questions the
 * design turns on:
 *
 *   1. After PG_CATCH, is the transaction usable without a subtransaction?
 *      (i.e. is BeginInternalSubTransaction actually required?)
 *   2. What exactly must be restored after a caught ERROR for the backend to
 *      keep working?
 *
 * Not part of the shipped runtime. Build: see Makefile target `probe`.
 */

#include "postgres.h"

#include "fmgr.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/builtins.h"
#include "executor/spi.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(fw_no_subxact);
PG_FUNCTION_INFO_V1(fw_with_subxact);
PG_FUNCTION_INFO_V1(fw_capture_only);

/*
 * The minimal PG_CATCH body: copy the error, flush the machinery.
 *
 * Returns true if the error was captured. `msg_out` receives a palloc'd copy
 * of the primary message, allocated in whatever context is current at catch
 * time (see the callers for why that matters).
 */
static bool
catch_minimal(char **msg_out)
{
    ErrorData *edata;

    edata = CopyErrorData();     /* must happen before FlushErrorState */
    *msg_out = pstrdup(edata->message);
    FlushErrorState();
    FreeErrorData(edata);
    return true;
}

/*
 * fw_no_subxact() — catch an ERROR with PG_TRY alone, then try to keep working.
 *
 * Deliberately does NOT use a subtransaction. The question is what the
 * backend reports when the caller then tries to allocate and query.
 */
Datum
fw_no_subxact(PG_FUNCTION_ARGS)
{
    char *msg = NULL;
    char *after = NULL;
    bool  caught = false;

    PG_TRY();
    {
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("fwprobe: deliberate error")));
    }
    PG_CATCH();
    {
        caught = catch_minimal(&msg);
    }
    PG_END_TRY();

    /*
     * The backend is still running — we got here. Now try to use it.
     *
     * The first thing that matters: can we allocate? palloc is the cheapest
     * possible operation that touches the memory-context machinery.
     */
    after = palloc(64);
    if (after != NULL)
        memcpy(after, "after-catch", 12);

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("no_subxact: caught=%s msg=\"%s\" palloc_ok=%s",
                 caught ? "yes" : "no",
                 msg ? msg : "(null)",
                 after ? "yes" : "no")));
}

/*
 * fw_with_subxact() — the plpgsql pattern: PG_TRY + internal subtransaction.
 *
 * This is the design the firewall will use when the extension asks for
 * continue-after-error semantics. Saves and restores the state PostgreSQL
 * does not restore for us.
 */
Datum
fw_with_subxact(PG_FUNCTION_ARGS)
{
    char *msg = NULL;
    bool  caught = false;

    MemoryContext oldcontext = CurrentMemoryContext;
    ResourceOwner oldowner = CurrentResourceOwner;

    BeginInternalSubTransaction(NULL);

    PG_TRY();
    {
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("fwprobe: deliberate error")));
    }
    PG_CATCH();
    {
        /*
         * Order matters. The error data must be copied into a context that
         * survives the rollback, so switch first, then copy.
         */
        MemoryContextSwitchTo(oldcontext);
        caught = catch_minimal(&msg);

        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_END_TRY();

    /*
     * ReleaseCurrentSubTransaction (on the success path) and the rollback
     * above both leave these two pointing at the subtransaction's objects.
     * Restoring them is not optional.
     */
    MemoryContextSwitchTo(oldcontext);
    CurrentResourceOwner = oldowner;

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("with_subxact: caught=%s msg=\"%s\" palloc_ok=%s",
                 caught ? "yes" : "no",
                 msg ? msg : "(null)",
                 palloc(64) ? "yes" : "no")));
}

/*
 * fw_capture_only() — the absolute minimum after a catch: copy and flush,
 * nothing else. Used to show what breaks when the restore steps are skipped.
 */
Datum
fw_capture_only(PG_FUNCTION_ARGS)
{
    char *msg = NULL;
    bool  caught = false;

    PG_TRY();
    {
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("fwprobe: deliberate error")));
    }
    PG_CATCH();
    {
        caught = catch_minimal(&msg);
    }
    PG_END_TRY();

    /* No context switch, no resource owner restore, no subtransaction. */
    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("capture_only: caught=%s msg=\"%s\"",
                 caught ? "yes" : "no", msg ? msg : "(null)")));
}


/*
 * The ordering rule, measured.
 *
 * CopyErrorData() pallocs into CurrentMemoryContext. At PG_CATCH time that
 * context is whatever the failing code left it as — it is not yours to assume.
 * If the failing code had switched into the subtransaction's context (which
 * SPI does: SPI_execute switches to the SPI proc context, a child of
 * CurTransactionContext), then copying AFTER the rollback allocates into freed
 * memory.
 *
 * A first version of this probe passed, which was misleading: it did no work
 * before erroring, so CurrentMemoryContext was still the caller's context and
 * survived the rollback by luck. These two probes differ only in whether SPI
 * ran first.
 */

/* Wrong order: error from inside SPI (context = subxact), then copy. */
PG_FUNCTION_INFO_V1(fw_copy_after_rollback);

Datum
fw_copy_after_rollback(PG_FUNCTION_ARGS)
{
    char *msg = NULL;
    bool  caught = false;

    MemoryContext oldcontext = CurrentMemoryContext;
    ResourceOwner oldowner = CurrentResourceOwner;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "fwprobe: SPI_connect failed");

    BeginInternalSubTransaction(NULL);

    PG_TRY();
    {
        /* This moves CurrentMemoryContext into the subtransaction. */
        SPI_execute("SELECT 1", true, 0);
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("fwprobe: deliberate error")));
    }
    PG_CATCH();
    {
        /* WRONG: roll back first, then copy. */
        RollbackAndReleaseCurrentSubTransaction();

        ErrorData *edata = CopyErrorData();
        FlushErrorState();
        msg = pstrdup(edata->message);
        FreeErrorData(edata);
        caught = true;
    }
    PG_END_TRY();

    MemoryContextSwitchTo(oldcontext);
    CurrentResourceOwner = oldowner;
    SPI_finish();

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("copy_after_rollback: caught=%s msg=\"%s\"",
                 caught ? "yes" : "no", msg ? msg : "(null)")));
}

/* Correct order: copy first, then roll back. */
PG_FUNCTION_INFO_V1(fw_copy_before_rollback);

Datum
fw_copy_before_rollback(PG_FUNCTION_ARGS)
{
    char *msg = NULL;
    bool  caught = false;

    MemoryContext oldcontext = CurrentMemoryContext;
    ResourceOwner oldowner = CurrentResourceOwner;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "fwprobe: SPI_connect failed");

    BeginInternalSubTransaction(NULL);

    PG_TRY();
    {
        SPI_execute("SELECT 1", true, 0);
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("fwprobe: deliberate error")));
    }
    PG_CATCH();
    {
        /* CORRECT: switch to a context we know survives, then copy. */
        MemoryContextSwitchTo(oldcontext);
        ErrorData *edata = CopyErrorData();
        FlushErrorState();
        msg = pstrdup(edata->message);
        FreeErrorData(edata);
        caught = true;

        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(oldcontext);
    CurrentResourceOwner = oldowner;
    SPI_finish();

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("copy_before_rollback: caught=%s msg=\"%s\"",
                 caught ? "yes" : "no", msg ? msg : "(null)")));
}
