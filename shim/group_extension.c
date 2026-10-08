/* group_extension.c — extension slots for the kwabi shim */

#include "shim_internal.h"

/*
 * NOTE ON SHAPE, and it is load-bearing.
 *
 * Every function here assigns its result inside the PG_TRY block and returns
 * it AFTER PG_END_TRY. Returning from between PG_TRY and PG_END_TRY would skip
 * the macro's restore of the global PG_exception_stack, leaving it aimed at a
 * stack frame that no longer exists. The backend then appears to work — until
 * the next ereport(ERROR) longjmps into the dead frame and segfaults. That is
 * not theoretical: it crashed this backend in extension-api and explain-api
 * until this shape was adopted. See notes/pg-try-return-hazard.md.
 */

static Oid
shim_extension_oid(const char *extname)
{
    Oid result = InvalidOid;

    if (extname == NULL)
        return InvalidOid;

    PG_TRY();
    {
        result = get_extension_oid(extname, false);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidOid;
    }
    PG_END_TRY();

    return result;
}

static bool
shim_extension_installed(const char *extname)
{
    bool result = false;

    if (extname == NULL)
        return false;

    PG_TRY();
    {
        result = OidIsValid(get_extension_oid(extname, true));
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = false;
    }
    PG_END_TRY();

    return result;
}

static const char *
shim_extension_version(const char *extname)
{
    const char *result = NULL;

    if (extname == NULL)
        return NULL;

    PG_TRY();
    {
        /* Look up the OID first (get_extension_oid is in commands/extension.h) */
        Oid oid = get_extension_oid(extname, true);

        if (OidIsValid(oid))
        {
            /* EXTENSIONOID syscache is available in all PG versions */
            HeapTuple tup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(oid));

            if (HeapTupleIsValid(tup))
            {
                bool isnull;
                Datum ver_datum = SysCacheGetAttr(EXTENSIONOID, tup,
                                                  Anum_pg_extension_extversion,
                                                  &isnull);
                if (!isnull)
                {
                    /* text_to_cstring pallocs into CurrentMemoryContext */
                    result = text_to_cstring(DatumGetTextPP(ver_datum));
                }
                ReleaseSysCache(tup);
            }
        }
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

void
init_group_extension(void)
{
    shim_table.extension_oid = shim_extension_oid;
    shim_table.extension_installed = shim_extension_installed;
    shim_table.extension_version = shim_extension_version;
}
