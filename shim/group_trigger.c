/* group_trigger.c — trigger slots for the kwabi shim */

#include "shim_internal.h"

/* ---- trigger slots --------------------------------------------------- */

TriggerDesc *
shim_trigger_desc(Oid relid)
{
    Relation rel = relation_open(relid, AccessShareLock);
    TriggerDesc *desc = rel->trigdesc;
    relation_close(rel, AccessShareLock);
    return desc;
}

int
shim_trigger_count(TriggerDesc desc)
{
    return desc.numtriggers;
}

Trigger
shim_trigger_get(TriggerDesc desc, int index)
{
    if (index < 0 || index >= desc.numtriggers)
        return (Trigger) {0};
    return desc.triggers[index];
}

void
init_group_trigger(void)
{
    shim_table.trigger_desc = shim_trigger_desc;
    shim_table.trigger_count = shim_trigger_count;
    shim_table.trigger_get = shim_trigger_get;
}
