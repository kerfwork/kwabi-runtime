/* group_relation.c — relation cache slots for the kwabi shim */

#include "shim_internal.h"

static LOCKMODE
kwabi_lockmode_to_pg(KwabiLockMode lockmode)
{
    switch (lockmode) {
        case KWABI_LOCKMODE_NONE:      return NoLock;
        case KWABI_LOCKMODE_SHARE:     return AccessShareLock;
        case KWABI_LOCKMODE_EXCLUSIVE: return AccessExclusiveLock;
        default:                       return AccessShareLock;
    }
}

static KwabiRelation
shim_relation_open(Oid relid, KwabiLockMode lockmode)
{
    KwabiRelation result = NULL;

    if (!OidIsValid(relid))
        return NULL;

    PG_TRY();
    {
        result = (KwabiRelation) relation_open(relid, kwabi_lockmode_to_pg(lockmode));
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

static void
shim_relation_close(KwabiRelation rel, KwabiLockMode lockmode)
{
    if (rel == NULL)
        return;

    PG_TRY();
    {
        relation_close((Relation) rel, kwabi_lockmode_to_pg(lockmode));
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static Oid
shim_relation_id(KwabiRelation rel)
{
    if (rel == NULL)
        return InvalidOid;
    return ((Relation) rel)->rd_id;
}

static const char *
shim_relation_name(KwabiRelation rel)
{
    if (rel == NULL)
        return NULL;
    return RelationGetRelationName((Relation) rel);
}

static Oid
shim_relation_namespace(KwabiRelation rel)
{
    if (rel == NULL)
        return InvalidOid;
    return ((Relation) rel)->rd_rel->relnamespace;
}

static TupleDesc
shim_relation_tupledesc(KwabiRelation rel)
{
    if (rel == NULL)
        return NULL;
    return ((Relation) rel)->rd_att;
}

static Oid
shim_rel_id(KwabiRelation rel)
{
    return shim_relation_id(rel);
}

static const char *
shim_rel_name(KwabiRelation rel)
{
    return shim_relation_name(rel);
}

static Oid
shim_rel_namespace(KwabiRelation rel)
{
    return shim_relation_namespace(rel);
}

static char
shim_rel_relkind(KwabiRelation rel)
{
    if (rel == NULL)
        return '\0';
    return ((Relation) rel)->rd_rel->relkind;
}

static Oid
shim_rel_relam(KwabiRelation rel)
{
    if (rel == NULL)
        return InvalidOid;
    return ((Relation) rel)->rd_rel->relam;
}

static TupleDesc
shim_rel_tupledesc(KwabiRelation rel)
{
    return shim_relation_tupledesc(rel);
}

static KwabiList
shim_rel_index_list(KwabiRelation rel)
{
    if (rel == NULL)
        return NULL;
    return (KwabiList) RelationGetIndexList((Relation) rel);
}

void
init_group_relation(void)
{
    shim_table.relation_open = shim_relation_open;
    shim_table.relation_close = shim_relation_close;
    shim_table.relation_id = shim_relation_id;
    shim_table.relation_name = shim_relation_name;
    shim_table.relation_namespace = shim_relation_namespace;
    shim_table.relation_tupledesc = shim_relation_tupledesc;
    shim_table.rel_id = shim_rel_id;
    shim_table.rel_name = shim_rel_name;
    shim_table.rel_namespace = shim_rel_namespace;
    shim_table.rel_relkind = shim_rel_relkind;
    shim_table.rel_relam = shim_rel_relam;
    shim_table.rel_tupledesc = shim_rel_tupledesc;
    shim_table.rel_index_list = shim_rel_index_list;
}
