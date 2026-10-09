/* group_tuple.c — tuple/slot slots for the kwabi shim */

#include "shim_internal.h"
#include "replication/slot.h"       /* SearchNamedReplicationSlot */
#include "utils/pg_lsn.h"           /* DatumGetLSN */

static int
shim_tuple_natts(TupleDesc tupdesc)
{
    if (tupdesc == NULL)
        return 0;
    return tupdesc->natts;
}

static Oid
shim_tuple_typeid(TupleDesc tupdesc, int attno)
{
    if (tupdesc == NULL || attno < 1 || attno > tupdesc->natts)
        return InvalidOid;
    return TupleDescAttr(tupdesc, attno - 1)->atttypid;
}

static int32
shim_tuple_typmod(TupleDesc tupdesc, int attno)
{
    if (tupdesc == NULL || attno < 1 || attno > tupdesc->natts)
        return -1;
    return TupleDescAttr(tupdesc, attno - 1)->atttypmod;
}

static const char *
shim_tuple_attname(TupleDesc tupdesc, int attno)
{
    if (tupdesc == NULL || attno < 1 || attno > tupdesc->natts)
        return NULL;
    return TupleDescAttr(tupdesc, attno - 1)->attname.data;
}

static bool
shim_tuple_attisdropped(TupleDesc tupdesc, int attno)
{
    if (tupdesc == NULL || attno < 1 || attno > tupdesc->natts)
        return false;
    return TupleDescAttr(tupdesc, attno - 1)->attisdropped;
}

static int
shim_tuple_attnum(TupleDesc tupdesc, const char *attname)
{
    int i;

    if (tupdesc == NULL || attname == NULL)
        return 0;

    for (i = 0; i < tupdesc->natts; i++)
    {
        if (strcmp(TupleDescAttr(tupdesc, i)->attname.data, attname) == 0)
            return i + 1;
    }
    return 0;
}

static Datum
shim_heap_tuple_getattr(HeapTuple tuple, int attno, TupleDesc tupdesc, bool *isnull)
{
    if (tuple == NULL || tupdesc == NULL || attno < 1 || attno > tupdesc->natts)
    {
        if (isnull != NULL)
            *isnull = true;
        return (Datum) 0;
    }
    return heap_getattr(tuple, attno, tupdesc, isnull);
}

static HeapTuple
shim_heap_tuple_setattr(HeapTuple tuple, int attno, Datum value, TupleDesc tupdesc)
{
    int natts;
    Datum *replValues;
    bool *replIsnull;
    bool *doReplace;
    HeapTuple newtuple;
    int i;

    if (tuple == NULL || tupdesc == NULL)
        return NULL;
    natts = tupdesc->natts;
    if (attno < 1 || attno > natts)
        return NULL;

    replValues = (Datum *) palloc(natts * sizeof(Datum));
    replIsnull = (bool *) palloc(natts * sizeof(bool));
    doReplace = (bool *) palloc(natts * sizeof(bool));

    for (i = 0; i < natts; i++)
    {
        doReplace[i] = (i == attno - 1);
        replValues[i] = value;
        replIsnull[i] = false;
    }

    newtuple = heap_modify_tuple(tuple, tupdesc, replValues, replIsnull, doReplace);
    pfree(replValues);
    pfree(replIsnull);
    pfree(doReplace);

    return newtuple;
}

static Oid
shim_heap_tuple_tableoid(HeapTuple tuple)
{
    if (tuple == NULL)
        return InvalidOid;
    return tuple->t_tableOid;
}

static ItemPointer
shim_heap_tuple_tid(HeapTuple tuple)
{
    if (tuple == NULL || tuple->t_data == NULL)
        return NULL;
    return &tuple->t_data->t_ctid;
}

static bool
shim_slot_isnull(KwabiSlot slot, int attno)
{
    TupleTableSlot *s = (TupleTableSlot *) slot;
    if (s == NULL || attno < 1 || attno > s->tts_tupleDescriptor->natts)
        return true;
    return s->tts_isnull[attno - 1];
}

static Datum
shim_slot_getattr(KwabiSlot slot, int attno, bool *isnull)
{
    TupleTableSlot *s = (TupleTableSlot *) slot;
    if (s == NULL || attno < 1 || attno > s->tts_tupleDescriptor->natts)
    {
        if (isnull != NULL)
            *isnull = true;
        return (Datum) 0;
    }
    return slot_getattr(s, attno, isnull);
}

static TupleDesc
shim_slot_tupledesc(KwabiSlot slot)
{
    TupleTableSlot *s = (TupleTableSlot *) slot;
    if (s == NULL)
        return NULL;
    return s->tts_tupleDescriptor;
}

static BlockNumber
shim_itempointer_get_block_number(ItemPointer pointer)
{
    if (pointer == NULL)
        return 0;
    return ItemPointerGetBlockNumber(pointer);
}

static BlockNumber
shim_block_get_number(ItemPointer pointer)
{
    if (pointer == NULL)
        return 0;
    return ItemPointerGetBlockNumber(pointer);
}

static OffsetNumber
shim_itempointer_get_offset_number(ItemPointer pointer)
{
    if (pointer == NULL)
        return 0;
    return ItemPointerGetOffsetNumber(pointer);
}

static OffsetNumber
shim_block_get_offset(ItemPointer pointer)
{
    if (pointer == NULL)
        return 0;
    return ItemPointerGetOffsetNumber(pointer);
}

/*
 * Replication slots are found by name. PostgreSQL gives them no OID, so the
 * ABI takes the name. Fields that the slot's spinlock protects are read under
 * it, and the control lock is held while the slot is looked up. A slot that
 * does not exist raises, after the lock is released.
 */
static void
shim_slot_read(const char *slot_name, XLogRecPtr *restart_lsn,
               TransactionId *catalog_xmin, bool *active)
{
    ReplicationSlot *slot;
    bool             found;

    if (slot_name == NULL)
        ereport(ERROR, (errmsg("kwabi: replication slot name is NULL")));

    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    slot = SearchNamedReplicationSlot(slot_name, false);
    found = (slot != NULL);
    if (found) {
        SpinLockAcquire(&slot->mutex);
        *restart_lsn = slot->data.restart_lsn;
        *catalog_xmin = slot->data.catalog_xmin;
        *active = (slot->active_pid != 0);
        SpinLockRelease(&slot->mutex);
    }
    LWLockRelease(ReplicationSlotControlLock);

    if (!found)
        ereport(ERROR, (errmsg("kwabi: replication slot \"%s\" does not exist", slot_name)));
}

static bool
shim_slot_is_active(const char *slot_name)
{
    XLogRecPtr      lsn;
    TransactionId   xmin;
    bool            active;

    shim_slot_read(slot_name, &lsn, &xmin, &active);
    return active;
}

static int64
shim_slot_get_lsn(const char *slot_name)
{
    XLogRecPtr      lsn;
    TransactionId   xmin;
    bool            active;

    shim_slot_read(slot_name, &lsn, &xmin, &active);
    return (int64) lsn;
}

static int64
shim_slot_get_catalog_xmin(const char *slot_name)
{
    XLogRecPtr      lsn;
    TransactionId   xmin;
    bool            active;

    shim_slot_read(slot_name, &lsn, &xmin, &active);
    return (int64) xmin;
}

static bool
shim_itempointer_is_valid(ItemPointer pointer)
{
    if (pointer == NULL)
        return false;
    return ItemPointerIsValid(pointer);
}

static bool
shim_block_is_valid(ItemPointer pointer)
{
    if (pointer == NULL)
        return false;
    return ItemPointerIsValid(pointer);
}

void
init_group_tuple(void)
{
    shim_table.tuple_natts = shim_tuple_natts;
    shim_table.tuple_typeid = shim_tuple_typeid;
    shim_table.tuple_typmod = shim_tuple_typmod;
    shim_table.tuple_attname = shim_tuple_attname;
    shim_table.tuple_attisdropped = shim_tuple_attisdropped;
    shim_table.tuple_attnum = shim_tuple_attnum;
    shim_table.heap_tuple_getattr = shim_heap_tuple_getattr;
    shim_table.heap_tuple_setattr = shim_heap_tuple_setattr;
    shim_table.heap_tuple_tableoid = shim_heap_tuple_tableoid;
    shim_table.heap_tuple_tid = shim_heap_tuple_tid;
    shim_table.slot_isnull = shim_slot_isnull;
    shim_table.slot_getattr = shim_slot_getattr;
    shim_table.slot_tupledesc = shim_slot_tupledesc;
    shim_table.slot_is_active = shim_slot_is_active;
    shim_table.slot_get_lsn = shim_slot_get_lsn;
    shim_table.slot_get_catalog_xmin = shim_slot_get_catalog_xmin;
    shim_table.block_get_number = shim_block_get_number;
    shim_table.itempointer_get_block_number = shim_itempointer_get_block_number;
    shim_table.itempointer_get_offset_number = shim_itempointer_get_offset_number;
    shim_table.itempointer_is_valid = shim_itempointer_is_valid;
    shim_table.block_get_offset = shim_block_get_offset;
    shim_table.block_is_valid = shim_block_is_valid;
}

/* ---- proof functions for the slot, block and item-pointer slots ------- */

/*
 * A physical replication slot the test owns. It is created with
 * immediately_reserve, so restart_lsn is set, and it is never acquired, so it
 * is inactive. Any slot left by an earlier run is dropped first.
 */
#define KWABI_TUPLE_SLOT "kwabi_tuple_slot"

static void
kwabi_tuple_slot_create(void)
{
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots "
                    "WHERE slot_name = '" KWABI_TUPLE_SLOT "'", false, 0) < 0 ||
        SPI_execute("SELECT pg_create_physical_replication_slot('" KWABI_TUPLE_SLOT "', true)",
                    false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: could not create the test replication slot")));
    }
    SPI_finish();
}

static void
kwabi_tuple_slot_drop(void)
{
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("SELECT pg_drop_replication_slot('" KWABI_TUPLE_SLOT "')", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: could not drop the test replication slot")));
    }
    SPI_finish();
}

/* Read one column of pg_replication_slots for the test slot, as a datum. */
static Datum
kwabi_tuple_slot_column(const char *column, bool *isnull)
{
    StringInfoData  sql;
    Datum           result = (Datum) 0;

    /* The query text lives in the SPI context, so build it before connecting. */
    initStringInfo(&sql);
    appendStringInfo(&sql, "SELECT %s FROM pg_replication_slots WHERE slot_name = '%s'",
                     column, KWABI_TUPLE_SLOT);

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute(sql.data, true, 0) < 0 || SPI_processed != 1) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: test replication slot not found in pg_replication_slots")));
    }
    result = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, isnull);
    /* The value is copied out by the caller before SPI_finish when it is
     * pass-by-value; restart_lsn and catalog_xmin are, so the copy is safe. */
    SPI_finish();
    return result;
}

/*
 * kwabi_slot_is_active_test() -> bool
 *
 * The test slot is inactive, and a name that does not exist must raise.
 */
PG_FUNCTION_INFO_V1(kwabi_slot_is_active_test);

Datum
kwabi_slot_is_active_test(PG_FUNCTION_ARGS)
{
    bool ok = false;

    if (shim_api == NULL || shim_api->slot_is_active == NULL)
        ereport(ERROR, (errmsg("kwabi: slot_is_active is not wired")));

    kwabi_tuple_slot_create();
    ok = (shim_api->slot_is_active(KWABI_TUPLE_SLOT) == false);

    PG_TRY();
    {
        (void) shim_api->slot_is_active("kwabi_no_such_slot");
        ok = false;
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        if (strstr(edata->message, "does not exist") == NULL)
            ok = false;
        FreeErrorData(edata);
        FlushErrorState();
    }
    PG_END_TRY();

    kwabi_tuple_slot_drop();
    PG_RETURN_BOOL(ok);
}

/*
 * kwabi_slot_get_lsn_test() -> bool
 *
 * slot_get_lsn returns the restart_lsn that pg_replication_slots reports.
 */
PG_FUNCTION_INFO_V1(kwabi_slot_get_lsn_test);

Datum
kwabi_slot_get_lsn_test(PG_FUNCTION_ARGS)
{
    bool isnull = true;
    bool ok;

    if (shim_api == NULL || shim_api->slot_get_lsn == NULL)
        ereport(ERROR, (errmsg("kwabi: slot_get_lsn is not wired")));

    kwabi_tuple_slot_create();
    {
        int64 from_abi = shim_api->slot_get_lsn(KWABI_TUPLE_SLOT);
        Datum sql_lsn = kwabi_tuple_slot_column("restart_lsn", &isnull);
        ok = (!isnull && from_abi != 0 && (XLogRecPtr) from_abi == DatumGetLSN(sql_lsn));
    }
    kwabi_tuple_slot_drop();
    PG_RETURN_BOOL(ok);
}

/*
 * kwabi_slot_get_catalog_xmin_test() -> bool
 *
 * A physical slot has no catalog_xmin: the ABI reports 0, and SQL reports NULL.
 */
PG_FUNCTION_INFO_V1(kwabi_slot_get_catalog_xmin_test);

Datum
kwabi_slot_get_catalog_xmin_test(PG_FUNCTION_ARGS)
{
    bool isnull = false;
    bool ok;

    if (shim_api == NULL || shim_api->slot_get_catalog_xmin == NULL)
        ereport(ERROR, (errmsg("kwabi: slot_get_catalog_xmin is not wired")));

    kwabi_tuple_slot_create();
    {
        int64 from_abi = shim_api->slot_get_catalog_xmin(KWABI_TUPLE_SLOT);
        (void) kwabi_tuple_slot_column("catalog_xmin", &isnull);
        ok = (from_abi == 0 && isnull);
    }
    kwabi_tuple_slot_drop();
    PG_RETURN_BOOL(ok);
}

/* ItemPointer (block, offset) = (5, 3), and an invalid one. */
#define KWABI_TID_BLOCK  ((BlockNumber) 5)
#define KWABI_TID_OFFSET ((OffsetNumber) 3)

static ItemPointerData
kwabi_test_tid(void)
{
    ItemPointerData tid;

    ItemPointerSet(&tid, KWABI_TID_BLOCK, KWABI_TID_OFFSET);
    return tid;
}

/*
 * kwabi_itempointer_get_block_number_test() -> bool
 * kwabi_itempointer_get_offset_number_test() -> bool
 * kwabi_itempointer_is_valid_test() -> bool
 *
 * The item pointer slots return the fields a pointer was set with, and report
 * an invalid pointer as invalid.
 */
PG_FUNCTION_INFO_V1(kwabi_itempointer_get_block_number_test);

Datum
kwabi_itempointer_get_block_number_test(PG_FUNCTION_ARGS)
{
    ItemPointerData tid = kwabi_test_tid();

    if (shim_api == NULL || shim_api->itempointer_get_block_number == NULL)
        ereport(ERROR, (errmsg("kwabi: itempointer_get_block_number is not wired")));
    PG_RETURN_BOOL(shim_api->itempointer_get_block_number(&tid) == KWABI_TID_BLOCK);
}

PG_FUNCTION_INFO_V1(kwabi_itempointer_get_offset_number_test);

Datum
kwabi_itempointer_get_offset_number_test(PG_FUNCTION_ARGS)
{
    ItemPointerData tid = kwabi_test_tid();

    if (shim_api == NULL || shim_api->itempointer_get_offset_number == NULL)
        ereport(ERROR, (errmsg("kwabi: itempointer_get_offset_number is not wired")));
    PG_RETURN_BOOL(shim_api->itempointer_get_offset_number(&tid) == KWABI_TID_OFFSET);
}

PG_FUNCTION_INFO_V1(kwabi_itempointer_is_valid_test);

Datum
kwabi_itempointer_is_valid_test(PG_FUNCTION_ARGS)
{
    ItemPointerData tid = kwabi_test_tid();
    ItemPointerData invalid;

    if (shim_api == NULL || shim_api->itempointer_is_valid == NULL)
        ereport(ERROR, (errmsg("kwabi: itempointer_is_valid is not wired")));
    ItemPointerSetInvalid(&invalid);
    PG_RETURN_BOOL(shim_api->itempointer_is_valid(&tid) && !shim_api->itempointer_is_valid(&invalid));
}

/*
 * kwabi_block_get_number_test() -> bool
 * kwabi_block_get_offset_test() -> bool
 * kwabi_block_is_valid_test() -> bool
 *
 * The block slots are the same accessors under their other name. They are
 * tested separately so each slot in the header has a check of its own.
 */
PG_FUNCTION_INFO_V1(kwabi_block_get_number_test);

Datum
kwabi_block_get_number_test(PG_FUNCTION_ARGS)
{
    ItemPointerData tid = kwabi_test_tid();

    if (shim_api == NULL || shim_api->block_get_number == NULL)
        ereport(ERROR, (errmsg("kwabi: block_get_number is not wired")));
    PG_RETURN_BOOL(shim_api->block_get_number(&tid) == KWABI_TID_BLOCK);
}

PG_FUNCTION_INFO_V1(kwabi_block_get_offset_test);

Datum
kwabi_block_get_offset_test(PG_FUNCTION_ARGS)
{
    ItemPointerData tid = kwabi_test_tid();

    if (shim_api == NULL || shim_api->block_get_offset == NULL)
        ereport(ERROR, (errmsg("kwabi: block_get_offset is not wired")));
    PG_RETURN_BOOL(shim_api->block_get_offset(&tid) == KWABI_TID_OFFSET);
}

PG_FUNCTION_INFO_V1(kwabi_block_is_valid_test);

Datum
kwabi_block_is_valid_test(PG_FUNCTION_ARGS)
{
    ItemPointerData tid = kwabi_test_tid();
    ItemPointerData invalid;

    if (shim_api == NULL || shim_api->block_is_valid == NULL)
        ereport(ERROR, (errmsg("kwabi: block_is_valid is not wired")));
    ItemPointerSetInvalid(&invalid);
    PG_RETURN_BOOL(shim_api->block_is_valid(&tid) && !shim_api->block_is_valid(&invalid));
}
