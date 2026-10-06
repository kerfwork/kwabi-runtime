/* group_syscache.c — syscache slots for the kwabi shim */

#include "shim_internal.h"

static enum SysCacheIdentifier
cache_name_to_id(const char *cache_name)
{
    if (cache_name == NULL)
        return (enum SysCacheIdentifier) -1;
    if (strcmp(cache_name, "TYPEOID") == 0)
        return TYPEOID;
    if (strcmp(cache_name, "OPEROID") == 0)
        return OPEROID;
    return (enum SysCacheIdentifier) -1;
}

static AttrNumber
type_attname_to_anum(const char *attname)
{
    if (attname == NULL)
        return 0;
    if (strcmp(attname, "typinput") == 0)
        return Anum_pg_type_typinput;
    if (strcmp(attname, "typoutput") == 0)
        return Anum_pg_type_typoutput;
    if (strcmp(attname, "typreceive") == 0)
        return Anum_pg_type_typreceive;
    if (strcmp(attname, "typsend") == 0)
        return Anum_pg_type_typsend;
    if (strcmp(attname, "typarray") == 0)
        return Anum_pg_type_typarray;
    if (strcmp(attname, "typbasetype") == 0)
        return Anum_pg_type_typbasetype;
    return 0;
}

static AttrNumber
oper_attname_to_anum(const char *attname)
{
    if (attname == NULL)
        return 0;
    if (strcmp(attname, "oprleft") == 0)
        return Anum_pg_operator_oprleft;
    if (strcmp(attname, "oprright") == 0)
        return Anum_pg_operator_oprright;
    if (strcmp(attname, "oprresult") == 0)
        return Anum_pg_operator_oprresult;
    if (strcmp(attname, "oprcom") == 0)
        return Anum_pg_operator_oprcom;
    return 0;
}

static Oid
shim_syscache_get_oid(const char *cache_name, const char *attname, Datum key)
{
    enum SysCacheIdentifier cacheid;
    AttrNumber attrnum;
    HeapTuple tup;
    Oid result = InvalidOid;
    bool isnull;

    cacheid = cache_name_to_id(cache_name);
    if (cacheid < 0)
        return InvalidOid;

    switch (cacheid) {
        case TYPEOID:
            attrnum = type_attname_to_anum(attname);
            break;
        case OPEROID:
            attrnum = oper_attname_to_anum(attname);
            break;
        default:
            return InvalidOid;
    }

    if (attrnum == 0)
        return InvalidOid;

    PG_TRY();
    {
        tup = SearchSysCache1(cacheid, key);
        if (HeapTupleIsValid(tup)) {
            result = DatumGetObjectId(SysCacheGetAttr(cacheid, tup, attrnum, &isnull));
            ReleaseSysCache(tup);
        }
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidOid;
    }
    PG_END_TRY();

    return result;
}

static HeapTuple
shim_syscache_get_tuple(const char *cache_name, Datum key)
{
    enum SysCacheIdentifier cacheid;
    HeapTuple result = NULL;

    cacheid = cache_name_to_id(cache_name);
    if (cacheid < 0)
        return NULL;

    PG_TRY();
    {
        result = SearchSysCache1(cacheid, key);
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
shim_syscache_free_tuple(HeapTuple tuple)
{
    if (tuple == NULL)
        return;
    ReleaseSysCache(tuple);
}

void
init_group_syscache(void)
{
    shim_table.syscache_get_oid = shim_syscache_get_oid;
    shim_table.syscache_get_tuple = shim_syscache_get_tuple;
    shim_table.syscache_free_tuple = shim_syscache_free_tuple;
}
