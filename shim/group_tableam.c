/* group_tableam.c — table access method slots for the kwabi shim */

#include "shim_internal.h"
#include "access/tableam.h"           /* table_tuple_*, table_beginscan, table_slot_create */
#include "executor/executor.h"        /* ExecClearTuple, ExecStoreVirtualTuple */
#include "executor/spi.h"            /* SPI_connect, SPI_execute */
#include "access/xact.h"             /* CommandCounterIncrement */

/*
 * A table AM handle is a Relation. The caller opens it with relation_open, and
 * every slot here takes that Relation. table_am_get returns the relation's
 * access method routine, for inspection only; it is not a substitute for the
 * Relation, and the slots below never treat it as one.
 */

/* ---- table AM slots --------------------------------------------------- */

KwabiTableAm
shim_table_am_get(KwabiRelation rel)
{
    if (rel == NULL)
        return NULL;
    return (KwabiTableAm) ((Relation) rel)->rd_tableam;
}

TableScanDesc
shim_table_am_beginscan(KwabiRelation rel, KwabiSnapshot snapshot, int nkeys, ScanKey key)
{
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: table_am_beginscan needs a relation")));
    return table_beginscan((Relation) rel, (Snapshot) snapshot, nkeys, key);
}

void
shim_table_am_endscan(TableScanDesc scan)
{
    if (scan != NULL)
        table_endscan(scan);
}

bool
shim_table_am_getnext(TableScanDesc scan, KwabiSlot slot)
{
    if (scan == NULL || slot == NULL)
        ereport(ERROR, (errmsg("kwabi: table_am_getnext needs a scan and a slot")));
    return table_scan_getnextslot(scan, ForwardScanDirection, (TupleTableSlot *) slot);
}

void
shim_table_am_insert(KwabiRelation rel, KwabiSlot slot, int options, BulkInsertState bistate)
{
    if (rel == NULL || slot == NULL)
        ereport(ERROR, (errmsg("kwabi: table_am_insert needs a relation and a slot")));
    table_tuple_insert((Relation) rel, (TupleTableSlot *) slot,
                       GetCurrentCommandId(true), (uint32) options, bistate);
}

/*
 * update and delete act on the row the slot was fetched from: the slot's tid
 * names it. options has no meaning for them yet, so anything but 0 raises.
 * A result other than TM_Ok (a concurrent update or delete, say) raises too,
 * rather than being dropped.
 */
void
shim_table_am_update(KwabiRelation rel, KwabiSlot slot, int options)
{
    ItemPointer      tid;
    TM_FailureData   tmfd;
    LockTupleMode    lockmode = LockTupleExclusive;
    TM_Result        result;
#if PG_VERSION_NUM >= 170000
    TU_UpdateIndexes update_indexes;
#else
    bool             update_indexes;
#endif

    if (rel == NULL || slot == NULL)
        ereport(ERROR, (errmsg("kwabi: table_am_update needs a relation and a slot")));
    if (options != 0)
        ereport(ERROR, (errmsg("kwabi: table_am_update takes no options yet")));

    tid = &((TupleTableSlot *) slot)->tts_tid;
    result = table_tuple_update((Relation) rel, tid, (TupleTableSlot *) slot,
                                GetCurrentCommandId(true), GetActiveSnapshot(),
                                InvalidSnapshot, true, &tmfd, &lockmode, &update_indexes);
    if (result != TM_Ok)
        ereport(ERROR, (errmsg("kwabi: table_am_update returned TM_Result %d", (int) result)));
}

void
shim_table_am_delete(KwabiRelation rel, KwabiSlot slot, int options)
{
    ItemPointer    tid;
    TM_FailureData tmfd;
    TM_Result      result;

    if (rel == NULL || slot == NULL)
        ereport(ERROR, (errmsg("kwabi: table_am_delete needs a relation and a slot")));
    if (options != 0)
        ereport(ERROR, (errmsg("kwabi: table_am_delete takes no options yet")));

    tid = &((TupleTableSlot *) slot)->tts_tid;
    result = table_tuple_delete((Relation) rel, tid, GetCurrentCommandId(true),
                                GetActiveSnapshot(), InvalidSnapshot, true, &tmfd, false);
    if (result != TM_Ok)
        ereport(ERROR, (errmsg("kwabi: table_am_delete returned TM_Result %d", (int) result)));
}

void
init_group_tableam(void)
{
    shim_table.table_am_get = shim_table_am_get;
    shim_table.table_am_beginscan = shim_table_am_beginscan;
    shim_table.table_am_endscan = shim_table_am_endscan;
    shim_table.table_am_getnext = shim_table_am_getnext;
    shim_table.table_am_insert = shim_table_am_insert;
    shim_table.table_am_update = shim_table_am_update;
    shim_table.table_am_delete = shim_table_am_delete;
}

/* ---- proof function --------------------------------------------------- */

#define KWABI_AM_TABLE "kwabi_am_t"

static Oid
kwabi_am_fixture_create(void)
{
    Oid relid;
    bool isnull;

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE IF EXISTS " KWABI_AM_TABLE, false, 0) < 0 ||
        SPI_execute("CREATE TABLE " KWABI_AM_TABLE " (v int4)", false, 0) < 0 ||
        SPI_execute("INSERT INTO " KWABI_AM_TABLE " VALUES (1), (2), (3)", false, 0) < 0 ||
        SPI_execute("SELECT '" KWABI_AM_TABLE "'::regclass::oid", true, 0) < 0 ||
        SPI_processed != 1) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: table AM fixture setup failed")));
    }
    relid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    SPI_finish();
    return relid;
}

/* The count and the sum of v, read by SQL. Both sides of each step use this. */
static void
kwabi_am_sql_state(int32 *count, int32 *sum)
{
    bool isnull;

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    CommandCounterIncrement();
    /* read_only would reuse the caller's snapshot, which predates this test's
     * inserts; a read-write run takes a fresh one. */
    if (SPI_execute("SELECT count(*)::int4, coalesce(sum(v), 0)::int4 FROM " KWABI_AM_TABLE,
                    false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: table AM state query failed")));
    }
    *count = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    *sum = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
    SPI_finish();
}

/*
 * kwabi_table_am_test() -> bool
 *
 * Scan, insert, update and delete a table through the table AM slots, with
 * each step's result checked against SQL. The slots take a Relation opened
 * with relation_open; the slots that the ABI does not yet provide (a slot
 * constructor) are built with PostgreSQL directly, and the test says so.
 */
PG_FUNCTION_INFO_V1(kwabi_table_am_test);

Datum
kwabi_table_am_test(PG_FUNCTION_ARGS)
{
    Oid           relid;
    KwabiRelation rel;
    KwabiTableAm  am;
    TableScanDesc scan;
    TupleTableSlot *slot;
    TupleTableSlot *dst;
    int32         count = 0, sum = 0;
    int           scanned = 0;
    bool          ok = false;

    if (shim_api == NULL || shim_api->relation_open == NULL ||
        shim_api->relation_close == NULL || shim_api->table_am_get == NULL ||
        shim_api->table_am_beginscan == NULL || shim_api->table_am_getnext == NULL ||
        shim_api->table_am_endscan == NULL || shim_api->table_am_insert == NULL ||
        shim_api->table_am_update == NULL || shim_api->table_am_delete == NULL)
        ereport(ERROR, (errmsg("kwabi: table AM slots are not wired")));

    relid = kwabi_am_fixture_create();
    rel = shim_api->relation_open(relid, KWABI_LOCKMODE_EXCLUSIVE);
    am = shim_api->table_am_get(rel);
    if (am != NULL && am == (KwabiTableAm) ((Relation) rel)->rd_tableam) {
        /* Slot for the scan and for building new rows; made with PostgreSQL. */
        slot = table_slot_create((Relation) rel, NULL);
        dst = table_slot_create((Relation) rel, NULL);

        /* Scan: three rows, before anything changes. */
        PushActiveSnapshot(GetTransactionSnapshot());
        scan = shim_api->table_am_beginscan(rel, GetActiveSnapshot(), 0, NULL);
        while (shim_api->table_am_getnext(scan, (KwabiSlot) slot))
            scanned++;
        shim_api->table_am_endscan(scan);
        PopActiveSnapshot();

        if (scanned == 3) {
            /* Insert v = 9 through the AM. */
            ExecClearTuple(dst);
            dst->tts_values[0] = Int32GetDatum(9);
            dst->tts_isnull[0] = false;
            ExecStoreVirtualTuple(dst);
            shim_api->table_am_insert(rel, (KwabiSlot) dst, 0, NULL);
            kwabi_am_sql_state(&count, &sum);
            ok = (count == 4 && sum == 15);
        }

        if (ok) {
            /* Update the first row (v = 1) to v = 42. Its tid is the slot's. */
            ok = false;
            PushActiveSnapshot(GetTransactionSnapshot());
            scan = shim_api->table_am_beginscan(rel, GetActiveSnapshot(), 0, NULL);
            if (shim_api->table_am_getnext(scan, (KwabiSlot) slot)) {
                slot_getallattrs(slot);
                ExecClearTuple(dst);
                dst->tts_values[0] = Int32GetDatum(42);
                dst->tts_isnull[0] = false;
                ExecStoreVirtualTuple(dst);
                dst->tts_tid = slot->tts_tid;
                shim_api->table_am_update(rel, (KwabiSlot) dst, 0);
                ok = true;
            }
            shim_api->table_am_endscan(scan);
            PopActiveSnapshot();
            kwabi_am_sql_state(&count, &sum);
            ok = ok && (count == 4 && sum == 9 + 2 + 3 + 42);
        }

        if (ok) {
            /* Delete the first row now in the heap (the updated v = 42 has moved). */
            ok = false;
            PushActiveSnapshot(GetTransactionSnapshot());
            scan = shim_api->table_am_beginscan(rel, GetActiveSnapshot(), 0, NULL);
            if (shim_api->table_am_getnext(scan, (KwabiSlot) slot)) {
                slot_getallattrs(slot);
                shim_api->table_am_delete(rel, (KwabiSlot) slot, 0);
                ok = true;
            }
            shim_api->table_am_endscan(scan);
            PopActiveSnapshot();
            kwabi_am_sql_state(&count, &sum);
            ok = ok && (count == 3);
        }
        ExecDropSingleTupleTableSlot(slot);
        ExecDropSingleTupleTableSlot(dst);
    }

    shim_api->relation_close(rel, KWABI_LOCKMODE_EXCLUSIVE);
    if (SPI_connect() == SPI_OK_CONNECT) {
        (void) SPI_execute("DROP TABLE IF EXISTS " KWABI_AM_TABLE, false, 0);
        SPI_finish();
    }
    PG_RETURN_BOOL(ok);
}
