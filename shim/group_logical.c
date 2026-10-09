/* group_logical.c — logical decoding slots for the kwabi shim
 *
 * Reads a logical slot through PostgreSQL's own SQL-level decoding functions,
 * pg_logical_slot_peek_changes and pg_replication_slot_advance. Nothing is
 * consumed by a read. A slot advances only when the caller confirms. The slot's
 * output plugin supplies the change format, so the shim needs no plugin of its
 * own and no WAL reader.
 */

#include "shim_internal.h"
#include "access/xlogdefs.h"          /* XLogRecPtr */
#include "executor/spi.h"             /* SPI_execute_with_args, SPI_getbinval */
#include "utils/builtins.h"           /* TextDatumGetCString */
#include "utils/memutils.h"           /* AllocSetContextCreate */
#include "utils/pg_lsn.h"             /* LSNGetDatum, DatumGetLSN */
#include "access/transam.h"           /* DatumGetTransactionId */

typedef struct KwabiLogicalRow
{
    XLogRecPtr      lsn;
    TransactionId   xid;
    char           *data;
} KwabiLogicalRow;

typedef struct KwabiLogicalDecodingImpl
{
    MemoryContext    cxt;           /* owns everything in this handle */
    char             slot_name[NAMEDATALEN];
    KwabiLogicalRow *rows;
    int              nrows;
    int              next;          /* index of the next row to read */
    XLogRecPtr       last_lsn;      /* highest LSN read so far; 0 if none */
} KwabiLogicalDecodingImpl;

/*
 * Peek every change from the slot's confirmed position into the handle's own
 * context, so the rows outlive the SPI connection. An unknown slot raises here,
 * from PostgreSQL, so begin fails early.
 */
static void
kwabi_logical_peek(KwabiLogicalDecodingImpl *d)
{
    Oid      argtypes[1] = { NAMEOID };
    Datum    args[1];
    char     nulls[1] = { ' ' };
    MemoryContext old;
    uint64   i;

    args[0] = DirectFunctionCall1(namein, CStringGetDatum(d->slot_name));

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute_with_args("SELECT lsn, xid, data FROM "
                              "pg_logical_slot_peek_changes($1, NULL, NULL)",
                              1, argtypes, args, nulls, false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: peeking changes from slot \"%s\" failed", d->slot_name)));
    }

    old = MemoryContextSwitchTo(d->cxt);
    d->nrows = (int) SPI_processed;
    d->rows = (KwabiLogicalRow *) palloc0(sizeof(KwabiLogicalRow) * Max(d->nrows, 1));
    for (i = 0; i < SPI_processed; i++) {
        bool isnull;
        HeapTuple tup = SPI_tuptable->vals[i];
        TupleDesc td = SPI_tuptable->tupdesc;

        d->rows[i].lsn = DatumGetLSN(SPI_getbinval(tup, td, 1, &isnull));
        d->rows[i].xid = DatumGetTransactionId(SPI_getbinval(tup, td, 2, &isnull));
        d->rows[i].data = TextDatumGetCString(SPI_getbinval(tup, td, 3, &isnull));
    }
    MemoryContextSwitchTo(old);
    SPI_finish();
}

static KwabiLogicalDecodingCtx
shim_logical_decoding_begin(const char *slot_name)
{
    MemoryContext             cxt;
    MemoryContext             old;
    KwabiLogicalDecodingImpl *d;

    if (slot_name == NULL)
        ereport(ERROR, (errmsg("kwabi: logical_decoding_begin needs a slot name")));
    if (strlen(slot_name) >= NAMEDATALEN)
        ereport(ERROR, (errmsg("kwabi: slot name \"%s\" is too long", slot_name)));

    cxt = AllocSetContextCreate(CurrentMemoryContext, "kwabi logical decoding",
                                ALLOCSET_SMALL_SIZES);
    old = MemoryContextSwitchTo(cxt);
    d = (KwabiLogicalDecodingImpl *) palloc0(sizeof(KwabiLogicalDecodingImpl));
    d->cxt = cxt;
    strlcpy(d->slot_name, slot_name, NAMEDATALEN);
    MemoryContextSwitchTo(old);

    kwabi_logical_peek(d);
    return (KwabiLogicalDecodingCtx) d;
}

static bool
shim_logical_decoding_read(KwabiLogicalDecodingCtx ctx, int64 *lsn, int32 *xid, const char **data)
{
    KwabiLogicalDecodingImpl *d = (KwabiLogicalDecodingImpl *) ctx;
    KwabiLogicalRow          *row;

    if (d == NULL || lsn == NULL || xid == NULL || data == NULL)
        ereport(ERROR, (errmsg("kwabi: logical_decoding_read needs a handle and out-parameters")));
    if (d->next >= d->nrows)
        return false;

    row = &d->rows[d->next++];
    *lsn = (int64) row->lsn;
    *xid = (int32) row->xid;
    *data = row->data;
    if (row->lsn > d->last_lsn)
        d->last_lsn = row->lsn;
    return true;
}

/*
 * Advance the slot to lsn. lsn may not be past the last change this handle has
 * read: a change the consumer never saw must not be skipped by a confirm.
 */
static void
shim_logical_decoding_confirm(KwabiLogicalDecodingCtx ctx, int64 lsn)
{
    KwabiLogicalDecodingImpl *d = (KwabiLogicalDecodingImpl *) ctx;
    Oid    argtypes[2] = { NAMEOID, PG_LSNOID };
    Datum  args[2];
    char   nulls[2] = { ' ', ' ' };

    if (d == NULL)
        ereport(ERROR, (errmsg("kwabi: logical_decoding_confirm needs a handle")));
    if (d->last_lsn == 0)
        ereport(ERROR, (errmsg("kwabi: nothing has been read from slot \"%s\" to confirm", d->slot_name)));
    if ((XLogRecPtr) lsn > d->last_lsn)
        ereport(ERROR, (errmsg("kwabi: confirm LSN %X/%X is past the last change read (%X/%X)",
                               (uint32) (lsn >> 32), (uint32) lsn,
                               (uint32) (d->last_lsn >> 32), (uint32) d->last_lsn)));

    args[0] = DirectFunctionCall1(namein, CStringGetDatum(d->slot_name));
    args[1] = LSNGetDatum((XLogRecPtr) lsn);

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute_with_args("SELECT pg_replication_slot_advance($1, $2)",
                              2, argtypes, args, nulls, false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: advancing slot \"%s\" failed", d->slot_name)));
    }
    SPI_finish();
}

static void
shim_logical_decoding_end(KwabiLogicalDecodingCtx ctx)
{
    KwabiLogicalDecodingImpl *d = (KwabiLogicalDecodingImpl *) ctx;

    if (d == NULL)
        return;
    MemoryContextDelete(d->cxt);
}

void
init_group_logical(void)
{
    shim_table.logical_decoding_begin = shim_logical_decoding_begin;
    shim_table.logical_decoding_read = shim_logical_decoding_read;
    shim_table.logical_decoding_confirm = shim_logical_decoding_confirm;
    shim_table.logical_decoding_end = shim_logical_decoding_end;
}

/* ---- proof functions ------------------------------------------------- */

/*
 * The tests run against data committed by logical-api.sql before they start:
 * two single-row transactions in kwabi_lg_t, decoded by test_decoding. A decode
 * cannot see uncommitted rows, so the test cannot write its own data.
 */
#define KWABI_LOGICAL_SLOT "kwabi_logical_t"

/* Count INSERT changes from a handle, and remember the last LSN and xid. */
static int
kwabi_logical_count_inserts(KwabiLogicalDecodingCtx ctx, int64 *last_lsn, bool *xid_seen)
{
    int64       lsn;
    int32       xid;
    const char *data;
    int         inserts = 0;

    while (shim_api->logical_decoding_read(ctx, &lsn, &xid, &data)) {
        if (strstr(data, "INSERT") != NULL)
            inserts++;
        if (xid != 0)
            *xid_seen = true;
        *last_lsn = lsn;
    }
    return inserts;
}

/*
 * kwabi_logical_read_test() -> bool
 *
 * Two committed inserts are read, in transactions with xids, and are not
 * consumed by the read. confirm past the last change raises. confirm to it
 * succeeds, and the slot's position moves.
 */
PG_FUNCTION_INFO_V1(kwabi_logical_read_test);

Datum
kwabi_logical_read_test(PG_FUNCTION_ARGS)
{
    KwabiLogicalDecodingCtx ctx;
    int64  last_lsn = 0;
    bool   xid_seen = false;
    bool   ok = false;
    int    inserts;
    bool   raised = false;

    if (shim_api == NULL || shim_api->logical_decoding_begin == NULL ||
        shim_api->logical_decoding_read == NULL || shim_api->logical_decoding_confirm == NULL ||
        shim_api->logical_decoding_end == NULL)
        ereport(ERROR, (errmsg("kwabi: logical decoding slots are not wired")));

    ctx = shim_api->logical_decoding_begin(KWABI_LOGICAL_SLOT);
    inserts = kwabi_logical_count_inserts(ctx, &last_lsn, &xid_seen);

    PG_TRY();
    {
        shim_api->logical_decoding_confirm(ctx, last_lsn + 1);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        raised = (strstr(edata->message, "past the last change") != NULL);
        FreeErrorData(edata);
        FlushErrorState();
    }
    PG_END_TRY();

    if (inserts == 2 && xid_seen && raised && last_lsn != 0) {
        shim_api->logical_decoding_confirm(ctx, last_lsn);
        ok = true;
    }
    shim_api->logical_decoding_end(ctx);
    PG_RETURN_BOOL(ok);
}

/*
 * kwabi_logical_after_confirm_test() -> bool
 *
 * After the confirm in kwabi_logical_read_test, a new handle sees no inserts.
 */
PG_FUNCTION_INFO_V1(kwabi_logical_after_confirm_test);

Datum
kwabi_logical_after_confirm_test(PG_FUNCTION_ARGS)
{
    KwabiLogicalDecodingCtx ctx;
    int64  last_lsn = 0;
    bool   xid_seen = false;
    int    inserts;

    ctx = shim_api->logical_decoding_begin(KWABI_LOGICAL_SLOT);
    inserts = kwabi_logical_count_inserts(ctx, &last_lsn, &xid_seen);
    shim_api->logical_decoding_end(ctx);
    PG_RETURN_BOOL(inserts == 0);
}

/*
 * kwabi_logical_missing_slot_test() -> bool
 *
 * begin on a slot that does not exist raises.
 */
PG_FUNCTION_INFO_V1(kwabi_logical_missing_slot_test);

Datum
kwabi_logical_missing_slot_test(PG_FUNCTION_ARGS)
{
    bool ok = false;

    PG_TRY();
    {
        KwabiLogicalDecodingCtx ctx = shim_api->logical_decoding_begin("kwabi_no_such_slot");
        shim_api->logical_decoding_end(ctx);
    }
    PG_CATCH();
    {
        FlushErrorState();
        ok = true;
    }
    PG_END_TRY();
    PG_RETURN_BOOL(ok);
}
