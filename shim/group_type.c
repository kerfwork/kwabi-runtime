/* group_type.c — type system slots for the kwabi shim */

#include "shim_internal.h"

/* Look up a type's I/O function OID from the catalog cache. */
static Oid
get_type_io_func(Oid type_oid, int which)
{
    HeapTuple tup;
    Oid func_oid = InvalidOid;
    bool isnull;

    if (!OidIsValid(type_oid))
        return InvalidOid;

    tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
    if (!HeapTupleIsValid(tup))
        return InvalidOid;

    switch (which) {
        case 0: /* input */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typinput, &isnull));
            break;
        case 1: /* output */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typoutput, &isnull));
            break;
        case 2: /* receive */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typreceive, &isnull));
            break;
        case 3: /* send */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typsend, &isnull));
            break;
    }

    ReleaseSysCache(tup);
    return func_oid;
}

static Datum
shim_type_input(Oid type_oid, const char *input, int32 typmod)
{
    Oid input_func;
    Datum result = (Datum) 0;

    if (!OidIsValid(type_oid) || input == NULL)
        return (Datum) 0;

    input_func = get_type_io_func(type_oid, 0);
    if (!OidIsValid(input_func))
        return (Datum) 0;

    PG_TRY();
    {
        result = OidInputFunctionCall(input_func, (char *) input, type_oid, typmod);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = (Datum) 0;
    }
    PG_END_TRY();

    return result;
}

static char *
shim_type_output(Oid type_oid, Datum value)
{
    Oid output_func;
    char *result = NULL;

    if (!OidIsValid(type_oid))
        return NULL;

    output_func = get_type_io_func(type_oid, 1);
    if (!OidIsValid(output_func))
        return NULL;

    PG_TRY();
    {
        result = OidOutputFunctionCall(output_func, value);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

static Datum
shim_type_recv(Oid type_oid, StringInfo buf)
{
    Oid recv_func;
    Datum result = (Datum) 0;

    if (!OidIsValid(type_oid) || buf == NULL)
        return (Datum) 0;

    recv_func = get_type_io_func(type_oid, 2);
    if (!OidIsValid(recv_func))
        return (Datum) 0;

    PG_TRY();
    {
        result = OidReceiveFunctionCall(recv_func, buf, type_oid, -1);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = (Datum) 0;
    }
    PG_END_TRY();

    return result;
}

static void
shim_type_send(Oid type_oid, Datum value, StringInfo buf)
{
    Oid send_func;
    bytea *result;

    if (!OidIsValid(type_oid) || buf == NULL)
        return;

    send_func = get_type_io_func(type_oid, 3);
    if (!OidIsValid(send_func))
        return;

    PG_TRY();
    {
        result = OidSendFunctionCall(send_func, value);
        if (result != NULL) {
            appendBinaryStringInfo(buf, VARDATA(result), VARSIZE(result) - VARHDRSZ);
        }
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static Oid
shim_type_element_type(Oid type_oid)
{
    Oid result = InvalidOid;

    if (!OidIsValid(type_oid))
        return InvalidOid;

    PG_TRY();
    {
        result = get_element_type(type_oid);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidOid;
    }
    PG_END_TRY();

    return result;
}

static int16
shim_type_length(Oid type_oid)
{
    int16 result = 0;

    if (!OidIsValid(type_oid))
        return 0;

    PG_TRY();
    {
        result = get_typlen(type_oid);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = 0;
    }
    PG_END_TRY();

    return result;
}

static bool
shim_type_is_array(Oid type_oid)
{
    bool result = false;

    if (!OidIsValid(type_oid))
        return false;

    PG_TRY();
    {
        result = OidIsValid(get_element_type(type_oid));
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = false;
    }
    PG_END_TRY();

    return result;
}

static bool
shim_type_is_composite(Oid type_oid)
{
    bool result = false;
    char typtype;

    if (!OidIsValid(type_oid))
        return false;

    PG_TRY();
    {
        typtype = get_typtype(type_oid);
        result = (typtype == TYPTYPE_COMPOSITE);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = false;
    }
    PG_END_TRY();

    return result;
}

static Oid
shim_type_base_type(Oid type_oid)
{
    Oid result = InvalidOid;

    if (!OidIsValid(type_oid))
        return InvalidOid;

    PG_TRY();
    {
        result = getBaseType(type_oid);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidOid;
    }
    PG_END_TRY();

    return result;
}

void
init_group_type(void)
{
    shim_table.type_input = shim_type_input;
    shim_table.type_output = shim_type_output;
    shim_table.type_recv = shim_type_recv;
    shim_table.type_send = shim_type_send;
    shim_table.type_element_type = shim_type_element_type;
    shim_table.type_length = shim_type_length;
    shim_table.type_is_array = shim_type_is_array;
    shim_table.type_is_composite = shim_type_is_composite;
    shim_table.type_base_type = shim_type_base_type;
}
