/* group_logical.c — logical decoding slots for the kwabi shim
 *
 * Reads a logical slot through PostgreSQL's SQL-level decoding functions,
 * pg_logical_slot_peek_binary_changes and pg_replication_slot_advance. Nothing
 * is consumed by a read. A slot advances only when the caller confirms. The
 * slot's output plugin supplies the format: test_decoding (text) or pgoutput
 * (binary messages). The shim needs no plugin of its own and no WAL reader.
 */

#include "shim_internal.h"
#include "access/transam.h"           /* DatumGetTransactionId */
#include "access/xlogdefs.h"          /* XLogRecPtr */
#include "executor/spi.h"             /* SPI_execute_with_args, SPI_getbinval */
#include "utils/array.h"              /* construct_md_array, construct_empty_array */
#include "utils/builtins.h"           /* DirectFunctionCall1, text helpers */
#include "utils/memutils.h"           /* AllocSetContextCreate */
#include "utils/pg_lsn.h"             /* LSNGetDatum, DatumGetLSN */

typedef struct KwabiLogicalRow
{
    XLogRecPtr      lsn;
    TransactionId   xid;
    char           *data;             /* NUL-terminated copy; len excludes the NUL */
    int32           len;
} KwabiLogicalRow;

typedef struct KwabiLogicalDecodingImpl
{
    MemoryContext    cxt;           /* owns everything in this handle */
    char             slot_name[NAMEDATALEN];
    KwabiLogicalRow *rows;
    int              nrows;
    int              next;          /* index of the next row to read */
    XLogRecPtr       last_lsn;      /* highest LSN read so far; 0 if none */
    int32            max_changes;   /* batch cap; 0 = no cap */
} KwabiLogicalDecodingImpl;

/*
 * The plugin options as the variadic text[] that pg_logical_slot_peek_* takes:
 * alternating name and value. With no options the array is empty.
 */
static ArrayType *
kwabi_logical_options_array(const char *const *names, const char *const *values, int32 noptions)
{
    Datum *elems;
    bool  *nulls;
    int    dims[1], lbs[1];
    int    i;

    if (noptions == 0)
        return construct_empty_array(TEXTOID);

    elems = (Datum *) palloc(sizeof(Datum) * 2 * noptions);
    nulls = (bool *) palloc0(sizeof(bool) * 2 * noptions);
    for (i = 0; i < noptions; i++) {
        if (names[i] == NULL || values[i] == NULL)
            ereport(ERROR, (errmsg("kwabi: logical decoding option %d has a NULL name or value", i)));
        elems[2 * i] = CStringGetTextDatum(names[i]);
        elems[2 * i + 1] = CStringGetTextDatum(values[i]);
    }
    dims[0] = 2 * noptions;
    lbs[0] = 1;
    return construct_md_array(elems, nulls, 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT);
}

/*
 * Peek every change from the slot's confirmed position into the handle's own
 * context, so the rows outlive the SPI connection. An unknown slot, or a plugin
 * that needs an option the caller did not give, raises here from PostgreSQL.
 */
static void
kwabi_logical_peek(KwabiLogicalDecodingImpl *d, const char *const *names,
                   const char *const *values, int32 noptions)
{
    Oid      argtypes[4] = { NAMEOID, INT4OID, TEXTARRAYOID };
    Datum    args[3];
    char     nulls[3] = { ' ', ' ', ' ' };
    ArrayType *options;
    MemoryContext old;
    MemoryContext oldcontext = CurrentMemoryContext;
    ResourceOwner oldowner = CurrentResourceOwner;
    uint64   i;

    args[0] = DirectFunctionCall1(namein, CStringGetDatum(d->slot_name));
    args[1] = Int32GetDatum(d->max_changes);
    if (d->max_changes == 0)
        nulls[1] = 'n';
    /* Built before SPI connects, in the caller's context: SPI_finish frees the SPI one. */
    options = kwabi_logical_options_array(names, values, noptions);
    args[2] = PointerGetDatum(options);

    /*
     * The SPI work runs in an internal subtransaction. A PostgreSQL error inside
     * it (an unknown slot, a plugin option it refuses) rolls the subtransaction
     * back, which releases the snapshots and SPI connection that SPI pushed, and
     * then raises the same error to the caller.
     */
    BeginInternalSubTransaction(NULL);
    PG_TRY();
    {
        if (SPI_connect() != SPI_OK_CONNECT)
            ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
        if (SPI_execute_with_args("SELECT lsn, xid, data FROM "
                                  "pg_logical_slot_peek_binary_changes($1, NULL, $2, VARIADIC $3)",
                                  3, argtypes, args, nulls, false, 0) < 0)
            ereport(ERROR, (errmsg("kwabi: peeking changes from slot \"%s\" failed", d->slot_name)));

        old = MemoryContextSwitchTo(d->cxt);
        d->nrows = (int) SPI_processed;
        d->rows = (KwabiLogicalRow *) palloc0(sizeof(KwabiLogicalRow) * Max(d->nrows, 1));
        for (i = 0; i < SPI_processed; i++) {
            bool      isnull;
            HeapTuple tup = SPI_tuptable->vals[i];
            TupleDesc td = SPI_tuptable->tupdesc;
            bytea    *raw;
            int       len;

            d->rows[i].lsn = DatumGetLSN(SPI_getbinval(tup, td, 1, &isnull));
            d->rows[i].xid = DatumGetTransactionId(SPI_getbinval(tup, td, 2, &isnull));
            raw = DatumGetByteaPP(SPI_getbinval(tup, td, 3, &isnull));
            len = (int) VARSIZE_ANY_EXHDR(raw);
            d->rows[i].data = (char *) palloc(len + 1);
            memcpy(d->rows[i].data, VARDATA_ANY(raw), len);
            d->rows[i].data[len] = '\0';
            d->rows[i].len = len;
        }
        MemoryContextSwitchTo(old);
        SPI_finish();
        ReleaseCurrentSubTransaction();
        CurrentResourceOwner = oldowner;
    }
    PG_CATCH();
    {
        ErrorData *edata;

        MemoryContextSwitchTo(oldcontext);
        edata = CopyErrorData();
        RollbackAndReleaseCurrentSubTransaction();
        CurrentResourceOwner = oldowner;
        ReThrowError(edata);
    }
    PG_END_TRY();
}

static KwabiLogicalDecodingCtx
shim_logical_decoding_begin(const char *slot_name, int32 max_changes,
                            const char *const *option_names, const char *const *option_values,
                            int32 noptions)
{
    MemoryContext             cxt;
    MemoryContext             old;
    KwabiLogicalDecodingImpl *d;

    if (slot_name == NULL)
        ereport(ERROR, (errmsg("kwabi: logical_decoding_begin needs a slot name")));
    if (strlen(slot_name) >= NAMEDATALEN)
        ereport(ERROR, (errmsg("kwabi: slot name \"%s\" is too long", slot_name)));
    if (noptions < 0 || (noptions > 0 && (option_names == NULL || option_values == NULL)))
        ereport(ERROR, (errmsg("kwabi: logical_decoding_begin needs option arrays for %d options", noptions)));
    if (max_changes < 0)
        ereport(ERROR, (errmsg("kwabi: max_changes must be 0 (no cap) or positive")));

    cxt = AllocSetContextCreate(CurrentMemoryContext, "kwabi logical decoding",
                                ALLOCSET_SMALL_SIZES);
    old = MemoryContextSwitchTo(cxt);
    d = (KwabiLogicalDecodingImpl *) palloc0(sizeof(KwabiLogicalDecodingImpl));
    d->cxt = cxt;
    strlcpy(d->slot_name, slot_name, NAMEDATALEN);
    d->max_changes = max_changes;
    MemoryContextSwitchTo(old);

    kwabi_logical_peek(d, option_names, option_values, noptions);
    return (KwabiLogicalDecodingCtx) d;
}

static bool
shim_logical_decoding_read(KwabiLogicalDecodingCtx ctx, int64 *lsn, int32 *xid,
                           const char **data, int32 *len)
{
    KwabiLogicalDecodingImpl *d = (KwabiLogicalDecodingImpl *) ctx;
    KwabiLogicalRow          *row;

    if (d == NULL || lsn == NULL || xid == NULL || data == NULL || len == NULL)
        ereport(ERROR, (errmsg("kwabi: logical_decoding_read needs a handle and out-parameters")));
    if (d->next >= d->nrows)
        return false;

    row = &d->rows[d->next++];
    *lsn = (int64) row->lsn;
    *xid = (int32) row->xid;
    *data = row->data;
    *len = row->len;
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
    MemoryContext oldcontext = CurrentMemoryContext;
    ResourceOwner oldowner = CurrentResourceOwner;

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

    BeginInternalSubTransaction(NULL);
    PG_TRY();
    {
        if (SPI_connect() != SPI_OK_CONNECT)
            ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
        if (SPI_execute_with_args("SELECT pg_replication_slot_advance($1, $2)",
                                  2, argtypes, args, nulls, false, 0) < 0)
            ereport(ERROR, (errmsg("kwabi: advancing slot \"%s\" failed", d->slot_name)));
        SPI_finish();
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;
    }
    PG_CATCH();
    {
        ErrorData *edata;

        MemoryContextSwitchTo(oldcontext);
        edata = CopyErrorData();
        RollbackAndReleaseCurrentSubTransaction();
        CurrentResourceOwner = oldowner;
        ReThrowError(edata);
    }
    PG_END_TRY();
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
 * two single-row transactions in kwabi_lg_t, decoded by test_decoding and by
 * pgoutput (publication kwabi_pub). A decode cannot see uncommitted rows, so the
 * tests cannot write their own data.
 */
#define KWABI_LOGICAL_SLOT "kwabi_logical_t"
#define KWABI_PGOUTPUT_SLOT "kwabi_pgo_t"

/* Count INSERT changes from a text plugin's handle, and track the last LSN and xid. */
static int
kwabi_logical_count_inserts(KwabiLogicalDecodingCtx ctx, int64 *last_lsn, bool *xid_seen)
{
    int64       lsn;
    int32       xid;
    const char *data;
    int32       len;
    int         inserts = 0;

    while (shim_api->logical_decoding_read(ctx, &lsn, &xid, &data, &len)) {
        if (strstr(data, "INSERT") != NULL)
            inserts++;
        if (xid != 0)
            *xid_seen = true;
        *last_lsn = lsn;
    }
    return inserts;
}

/*
 * kwabi_logical_batch_test() -> bool
 *
 * A capped handle ends at a transaction boundary: a cap of 2 over a fixture of
 * two-row transactions (BEGIN, INSERT, COMMIT each) returns the whole first
 * transaction, three rows. With no confirm the next handle returns the same first
 * batch again: the batch is replayed, not consumed. Runs before the read test,
 * while the fixture is unconfirmed.
 */
PG_FUNCTION_INFO_V1(kwabi_logical_batch_test);

Datum
kwabi_logical_batch_test(PG_FUNCTION_ARGS)
{
    KwabiLogicalDecodingCtx ctx;
    int64       lsn1 = 0, lsn2 = 0;
    int32       xid, len;
    const char *data;
    int         first = 0, second = 0;
    bool        ok;

    ctx = shim_api->logical_decoding_begin(KWABI_LOGICAL_SLOT, 2, NULL, NULL, 0);
    while (shim_api->logical_decoding_read(ctx, &lsn1, &xid, &data, &len))
        first++;
    shim_api->logical_decoding_end(ctx);

    ctx = shim_api->logical_decoding_begin(KWABI_LOGICAL_SLOT, 2, NULL, NULL, 0);
    if (shim_api->logical_decoding_read(ctx, &lsn2, &xid, &data, &len))
        second = 1;
    shim_api->logical_decoding_end(ctx);

    /* The cap of 2 closed at the first COMMIT: BEGIN, INSERT, COMMIT. */
    ok = (first == 3 && second == 1 && lsn2 != 0);
    PG_RETURN_BOOL(ok);
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

    ctx = shim_api->logical_decoding_begin(KWABI_LOGICAL_SLOT, 0, NULL, NULL, 0);
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

    ctx = shim_api->logical_decoding_begin(KWABI_LOGICAL_SLOT, 0, NULL, NULL, 0);
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
        KwabiLogicalDecodingCtx ctx =
            shim_api->logical_decoding_begin("kwabi_no_such_slot", 0, NULL, NULL, 0);
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

/*
 * kwabi_pgoutput_test() -> bool
 *
 * pgoutput, with its required options, yields binary protocol messages. The
 * fixture has two committed transactions, each one insert, so the stream holds
 * two Begin ('B'), two Insert ('I') and two Commit ('C') messages, and a Relation
 * ('R') message describing kwabi_lg_t. The tags are the first byte of each message.
 */
PG_FUNCTION_INFO_V1(kwabi_pgoutput_test);

Datum
kwabi_pgoutput_test(PG_FUNCTION_ARGS)
{
    static const char *names[] = { "proto_version", "publication_names" };
    static const char *values[] = { "1", "kwabi_pub" };
    KwabiLogicalDecodingCtx ctx;
    int64       lsn;
    int32       xid, len;
    const char *data;
    int         begins = 0, inserts = 0, commits = 0, relations = 0;
    bool        ok;

    ctx = shim_api->logical_decoding_begin(KWABI_PGOUTPUT_SLOT, 0, names, values, 2);
    while (shim_api->logical_decoding_read(ctx, &lsn, &xid, &data, &len)) {
        if (len < 1)
            continue;
        switch (data[0]) {
            case 'B': begins++; break;
            case 'I': inserts++; break;
            case 'C': commits++; break;
            case 'R': relations++; break;
            default: break;
        }
    }
    shim_api->logical_decoding_end(ctx);

    ok = (begins == 2 && inserts == 2 && commits == 2 && relations >= 1);
    PG_RETURN_BOOL(ok);
}

/*
 * kwabi_pgoutput_no_options_test() -> bool
 *
 * pgoutput without its options raises: a plugin that needs an option must not
 * start with a silent default.
 */
PG_FUNCTION_INFO_V1(kwabi_pgoutput_no_options_test);

Datum
kwabi_pgoutput_no_options_test(PG_FUNCTION_ARGS)
{
    bool ok = false;

    PG_TRY();
    {
        KwabiLogicalDecodingCtx ctx =
            shim_api->logical_decoding_begin(KWABI_PGOUTPUT_SLOT, 0, NULL, NULL, 0);
        shim_api->logical_decoding_end(ctx);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        ok = (strstr(edata->message, "proto_version") != NULL);
        FreeErrorData(edata);
        FlushErrorState();
    }
    PG_END_TRY();
    PG_RETURN_BOOL(ok);
}

/*
 * Two-phase commit through pgoutput. The SQL fixture prepares and commits or
 * rolls back transactions, because PREPARE TRANSACTION cannot run inside a
 * function. These tests only read what the fixture left in the slot, in order:
 *
 *   kwabi_2pc_prepared_test  after PREPARE: data is delivered (I, P), no commit (C, K)
 *   kwabi_2pc_commit_test    after COMMIT PREPARED: K arrives; the read is confirmed
 *   kwabi_2pc_rollback_test  after ROLLBACK PREPARED: r arrives, and no K
 */
#define KWABI_2PC_SLOT "kwabi_2pc_t"

static void
kwabi_2pc_tags(KwabiLogicalDecodingCtx ctx, int *begin_prepare, int *inserts,
               int *prepares, int *commits, int *commit_prepared, int *rollback_prepared,
               int64 *last_lsn)
{
    int64       lsn;
    int32       xid, len;
    const char *data;

    while (shim_api->logical_decoding_read(ctx, &lsn, &xid, &data, &len)) {
        if (len < 1)
            continue;
        switch (data[0]) {
            case 'b': (*begin_prepare)++; break;
            case 'I': (*inserts)++; break;
            case 'P': (*prepares)++; break;
            case 'C': (*commits)++; break;
            case 'K': (*commit_prepared)++; break;
            case 'r': (*rollback_prepared)++; break;
            default: break;
        }
        *last_lsn = lsn;
    }
}

static const char *kwabi_2pc_names[] = { "proto_version", "two_phase", "publication_names" };
static const char *kwabi_2pc_values[] = { "3", "on", "kwabi_pub" };

PG_FUNCTION_INFO_V1(kwabi_2pc_prepared_test);

Datum
kwabi_2pc_prepared_test(PG_FUNCTION_ARGS)
{
    KwabiLogicalDecodingCtx ctx;
    int begin_prepare = 0, inserts = 0, prepares = 0, commits = 0, commit_prepared = 0, rollback_prepared = 0;
    int64 last_lsn = 0;
    bool  ok;

    ctx = shim_api->logical_decoding_begin(KWABI_2PC_SLOT, 0, kwabi_2pc_names, kwabi_2pc_values, 3);
    kwabi_2pc_tags(ctx, &begin_prepare, &inserts, &prepares, &commits, &commit_prepared, &rollback_prepared, &last_lsn);
    shim_api->logical_decoding_end(ctx);

    /* The prepared transaction's data is out before it is final. */
    ok = (begin_prepare == 1 && inserts == 1 && prepares == 1 &&
          commits == 0 && commit_prepared == 0 && rollback_prepared == 0);
    PG_RETURN_BOOL(ok);
}

PG_FUNCTION_INFO_V1(kwabi_2pc_commit_test);

Datum
kwabi_2pc_commit_test(PG_FUNCTION_ARGS)
{
    KwabiLogicalDecodingCtx ctx;
    int begin_prepare = 0, inserts = 0, prepares = 0, commits = 0, commit_prepared = 0, rollback_prepared = 0;
    int64 last_lsn = 0;
    bool  ok;

    ctx = shim_api->logical_decoding_begin(KWABI_2PC_SLOT, 0, kwabi_2pc_names, kwabi_2pc_values, 3);
    kwabi_2pc_tags(ctx, &begin_prepare, &inserts, &prepares, &commits, &commit_prepared, &rollback_prepared, &last_lsn);

    /* Commit is the outcome: K, and never a plain C for a prepared transaction. */
    ok = (commit_prepared == 1 && commits == 0 && rollback_prepared == 0 && last_lsn != 0);
    if (ok)
        shim_api->logical_decoding_confirm(ctx, last_lsn);
    shim_api->logical_decoding_end(ctx);
    PG_RETURN_BOOL(ok);
}

PG_FUNCTION_INFO_V1(kwabi_2pc_rollback_test);

Datum
kwabi_2pc_rollback_test(PG_FUNCTION_ARGS)
{
    KwabiLogicalDecodingCtx ctx;
    int begin_prepare = 0, inserts = 0, prepares = 0, commits = 0, commit_prepared = 0, rollback_prepared = 0;
    int64 last_lsn = 0;
    bool  ok;

    ctx = shim_api->logical_decoding_begin(KWABI_2PC_SLOT, 0, kwabi_2pc_names, kwabi_2pc_values, 3);
    kwabi_2pc_tags(ctx, &begin_prepare, &inserts, &prepares, &commits, &commit_prepared, &rollback_prepared, &last_lsn);
    shim_api->logical_decoding_end(ctx);

    /* The read starts after the commit test's confirm, so only the rollback is new. */
    ok = (rollback_prepared == 1 && commit_prepared == 0 && commits == 0);
    PG_RETURN_BOOL(ok);
}
