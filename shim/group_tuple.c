/* group_tuple.c — tuple/slot slots for the kwabi shim */

#include "shim_internal.h"

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
}
