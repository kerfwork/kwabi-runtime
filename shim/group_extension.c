/* group_extension.c — extension slots for the kwabi shim */

#include "shim_internal.h"

static Oid
shim_extension_oid(const char *extname)
{
    if (extname == NULL)
        return InvalidOid;

    PG_TRY();
    {
        Oid result = get_extension_oid(extname, false);
        return result;
    }
    PG_CATCH();
    {
        shim_capture_error();
        return InvalidOid;
    }
    PG_END_TRY();
}

static bool
shim_extension_installed(const char *extname)
{
    if (extname == NULL)
        return false;

    PG_TRY();
    {
        Oid oid = get_extension_oid(extname, true);
        return OidIsValid(oid);
    }
    PG_CATCH();
    {
        shim_capture_error();
        return false;
    }
    PG_END_TRY();
}

static const char *
shim_extension_version(const char *extname)
{
    if (extname == NULL)
        return NULL;

    PG_TRY();
    {
        /* Look up the OID first (get_extension_oid is in commands/extension.h) */
        Oid oid = get_extension_oid(extname, true);
        if (!OidIsValid(oid))
            return NULL;

        /* Now use EXTENSIONOID syscache (available in all PG versions) */
        HeapTuple tup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(oid));
        if (!HeapTupleIsValid(tup))
            return NULL;

        bool isnull;
        Datum ver_datum = SysCacheGetAttr(EXTENSIONOID, tup,
                                          Anum_pg_extension_extversion,
                                          &isnull);
        if (isnull) {
            ReleaseSysCache(tup);
            return NULL;
        }

        /* text_to_cstring pallocs into CurrentMemoryContext */
        const char *ver = text_to_cstring(DatumGetTextPP(ver_datum));
        ReleaseSysCache(tup);
        return ver;
    }
    PG_CATCH();
    {
        shim_capture_error();
        return NULL;
    }
    PG_END_TRY();
}

void
init_group_extension(void)
{
    shim_table.extension_oid = shim_extension_oid;
    shim_table.extension_installed = shim_extension_installed;
    shim_table.extension_version = shim_extension_version;
}
