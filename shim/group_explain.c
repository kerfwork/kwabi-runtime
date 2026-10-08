/* group_explain.c — explain proof functions for the kwabi shim */

#include "shim_internal.h"

/* ========================================================================
 * Explain proof functions
 * ======================================================================== */

/*
 * shim_explain_get_index_name - get an index name through the ABI
 *
 * Result is assigned inside PG_TRY and returned AFTER PG_END_TRY. Returning
 * from between the two would skip the restore of PG_exception_stack and crash
 * the next ereport. See group_extension.c for the full note.
 */
static const char *
shim_explain_get_index_name(Oid indexOid)
{
    const char *result = NULL;

    if (!OidIsValid(indexOid))
        return NULL;

    PG_TRY();
    {
        Relation indexRel = relation_open(indexOid, AccessShareLock);
        result = pstrdup(RelationGetRelationName(indexRel));
        relation_close(indexRel, AccessShareLock);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

/*
 * kwabi_explain_get_index_name_test(int4) -> text
 *
 * Test explain_get_index_name through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_explain_get_index_name_test);

Datum
kwabi_explain_get_index_name_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->explain_get_index_name == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_get_index_name is not wired")));

    Oid indexOid = (Oid) PG_GETARG_INT32(0);
    const char *name = shim_api->explain_get_index_name(indexOid);

    PG_RETURN_TEXT_P(cstring_to_text(name ? name : "(null)"));
}

/*
 * kwabi_explain_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts a wrong index name, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_explain_control);

Datum
kwabi_explain_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->explain_get_index_name == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_get_index_name is not wired")));

    const char *name = shim_api->explain_get_index_name(1259); /* pg_class */

    /* The control's whole point: this comparison must be FALSE. */
    if (name != NULL && strcmp(name, "wrong_index_name") == 0)
        PG_RETURN_BOOL(true);   /* the comparison thinks pg_class is wrong_index_name: broken */

    ereport(ERROR,
            (errmsg("kwabi: explain negative control fired as intended"),
             errdetail("index name is not wrong_index_name -- the value comparison is honest")));
}

void
init_group_explain(void)
{
    shim_table.explain_get_index_name = shim_explain_get_index_name;
}
