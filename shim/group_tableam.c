/* group_tableam.c — table access method slots for the kwabi shim */

#include "shim_internal.h"

/* ---- table AM slots --------------------------------------------------- */

KwabiTableAm
shim_table_am_get(Oid relid)
{
    Relation rel = relation_open(relid, AccessShareLock);
    KwabiTableAm am = (KwabiTableAm) rel->rd_tableam;
    relation_close(rel, AccessShareLock);
    return am;
}

TableScanDesc
shim_table_am_beginscan(KwabiTableAm am, Snapshot snapshot, int nkeys, ScanKey key)
{
    return table_beginscan(am, snapshot, nkeys, key);
}

void
shim_table_am_endscan(TableScanDesc scan)
{
    table_endscan(scan);
}

bool
shim_table_am_getnext(TableScanDesc scan, KwabiSlot slot)
{
    return table_scan_getnextslot(scan, ForwardScanDirection, (TupleTableSlot *) slot);
}

void
shim_table_am_insert(KwabiTableAm am, KwabiSlot slot, int options, BulkInsertState bistate)
{
    table_tuple_insert((Relation) am, (TupleTableSlot *) slot, GetCurrentCommandId(true), options, bistate);
}

void
shim_table_am_update(KwabiTableAm am, KwabiSlot slot, int options)
{
    ItemPointer tid = &(((TupleTableSlot *) slot)->tts_tid);
    TM_FailureData tmfd;
    LockTupleMode lockmode = LockTupleExclusive;
    TU_UpdateIndexes update_indexes;
    table_tuple_update((Relation) am, tid, (TupleTableSlot *) slot, GetCurrentCommandId(true), GetActiveSnapshot(), InvalidSnapshot, true, &tmfd, &lockmode, &update_indexes);
}

void
shim_table_am_delete(KwabiTableAm am, KwabiSlot slot, int options)
{
    ItemPointer tid = &(((TupleTableSlot *) slot)->tts_tid);
    TM_FailureData tmfd;
    table_tuple_delete((Relation) am, tid, GetCurrentCommandId(true), GetActiveSnapshot(), InvalidSnapshot, true, &tmfd, false);
}

void
init_group_tableam(void)
{
    shim_table.table_am_get = shim_table_am_get;
    shim_table.table_am_beginscan = (TableScanDesc (*)(KwabiTableAm, KwabiSnapshot, int, ScanKey)) shim_table_am_beginscan;
    shim_table.table_am_endscan = shim_table_am_endscan;
    shim_table.table_am_getnext = shim_table_am_getnext;
    shim_table.table_am_insert = shim_table_am_insert;
    shim_table.table_am_update = shim_table_am_update;
    shim_table.table_am_delete = shim_table_am_delete;
}
