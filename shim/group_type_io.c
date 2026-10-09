/* group_type_io.c — SQL-callable text I/O for types whose bodies are reloadable.
 *
 * The bundle exports two C functions, kwabi_type_in and kwabi_type_out. A type named
 * <name> is created with functions <name>_in and <name>_out that both point at these
 * symbols. The function's own name, read from its OID, gives the binding to use, so
 * one symbol serves every type. The bodies live in the bound library
 * (see KwabiTypeBodies in kwabi.h), and a reload replaces them.
 */

#include "shim_internal.h"
#include "catalog/pg_proc.h"
#include "utils/lsyscache.h"

#define KWABI_TYPE_OUTPUT_MAX 32

/* The binding name for the SQL function that is running: its name minus `suffix`. */
static char *
binding_name(Oid fn_oid, const char *suffix)
{
    char       *fname = get_func_name(fn_oid);
    size_t      len;

    if (fname == NULL)
        ereport(ERROR, (errmsg("kwabi: cannot find the name of the running function")));
    len = strlen(fname);
    if (len <= strlen(suffix) || strcmp(fname + len - strlen(suffix), suffix) != 0)
        ereport(ERROR, (errmsg("kwabi: function %s does not end in %s", fname, suffix)));
    fname[len - strlen(suffix)] = '\0';
    return fname;
}

PG_FUNCTION_INFO_V1(kwabi_type_in);
Datum
kwabi_type_in(PG_FUNCTION_ARGS)
{
    char       *text = PG_GETARG_CSTRING(0);
    char       *name = binding_name(fcinfo->flinfo->fn_oid, "_in");
    KwabiTypeBodies bodies;
    KwabiError  err;
    uint64      value = 0;
    KwabiStatus st;

    if (!shim_type_bodies_lookup(name, &bodies))
        ereport(ERROR, (errmsg("kwabi type \"%s\" is not bound to a library with a type table",
                               name)));

    kwabi_error_init(&err);
    st = bodies.input(text, &value, &err, bodies.arg);
    if (st != KWABI_OK)
    {
        if (err.sqlerrcode == 0)
            kwabi_error_set_core(&err, ERRCODE_INVALID_TEXT_REPRESENTATION,
                                 KWABI_ERR_BODY_RAISED, "invalid input syntax");
        shim_raise_kwabi_error(&err);
    }
    PG_RETURN_UINT64(value);
}

PG_FUNCTION_INFO_V1(kwabi_type_out);
Datum
kwabi_type_out(PG_FUNCTION_ARGS)
{
    uint64      value = PG_GETARG_INT64(0);
    char       *name = binding_name(fcinfo->flinfo->fn_oid, "_out");
    KwabiTypeBodies bodies;
    KwabiError  err;
    char       *buf;
    KwabiStatus st;

    if (!shim_type_bodies_lookup(name, &bodies))
        ereport(ERROR, (errmsg("kwabi type \"%s\" is not bound to a library with a type table",
                               name)));

    buf = palloc(KWABI_TYPE_OUTPUT_MAX);
    kwabi_error_init(&err);
    st = bodies.output(value, buf, KWABI_TYPE_OUTPUT_MAX, &err, bodies.arg);
    if (st != KWABI_OK)
    {
        if (err.sqlerrcode == 0)
            kwabi_error_set_core(&err, ERRCODE_INTERNAL_ERROR,
                                 KWABI_ERR_BODY_RAISED, "output failed");
        shim_raise_kwabi_error(&err);
    }
    PG_RETURN_CSTRING(buf);
}

/* The operator for the running function: the part of its name after the last '_'. */
static uint32
operator_code(const char *op, bool *is_bool, bool *is_order)
{
    struct
    {
        const char *name;
        uint32      code;
    }           table[] = {
        {"eq", KWABI_TYPE_OP_EQ}, {"ne", KWABI_TYPE_OP_NE},
        {"lt", KWABI_TYPE_OP_LT}, {"le", KWABI_TYPE_OP_LE},
        {"gt", KWABI_TYPE_OP_GT}, {"ge", KWABI_TYPE_OP_GE},
        {"add", KWABI_TYPE_OP_ADD}, {"sub", KWABI_TYPE_OP_SUB},
        {"mul", KWABI_TYPE_OP_MUL}, {"div", KWABI_TYPE_OP_DIV},
        {"mod", KWABI_TYPE_OP_MOD}, {"cmp", KWABI_TYPE_OP_ORDER},
    };

    *is_bool = false;
    *is_order = false;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        if (strcmp(op, table[i].name) == 0)
        {
            *is_bool = table[i].code <= KWABI_TYPE_OP_GE;
            *is_order = table[i].code == KWABI_TYPE_OP_ORDER;
            return table[i].code;
        }
    }
    ereport(ERROR, (errmsg("kwabi: unknown type operator \"%s\"", op)));
    return 0;                   /* not reached */
}

PG_FUNCTION_INFO_V1(kwabi_type_binop);
Datum
kwabi_type_binop(PG_FUNCTION_ARGS)
{
    uint64      a = (uint64) PG_GETARG_INT64(0);
    uint64      b = (uint64) PG_GETARG_INT64(1);
    char       *fname = get_func_name(fcinfo->flinfo->fn_oid);
    char       *us;
    char       *name;
    uint32      code;
    bool        is_bool;
    bool        is_order;
    KwabiTypeBodies bodies;
    KwabiError  err;
    uint64      result = 0;
    KwabiStatus st;

    if (fname == NULL || (us = strrchr(fname, '_')) == NULL)
        ereport(ERROR, (errmsg("kwabi: operator function name has no _<op> suffix")));
    name = pnstrdup(fname, us - fname);
    code = operator_code(us + 1, &is_bool, &is_order);

    if (!shim_type_bodies_lookup(name, &bodies))
        ereport(ERROR, (errmsg("kwabi type \"%s\" is not bound to a library with a type table",
                               name)));
    if (bodies.binop == NULL)
        ereport(ERROR, (errmsg("kwabi type \"%s\" has no operator bodies", name)));

    kwabi_error_init(&err);
    st = bodies.binop(code, a, b, &result, &err, bodies.arg);
    if (st != KWABI_OK)
    {
        if (err.sqlerrcode == 0)
            kwabi_error_set_core(&err, ERRCODE_INTERNAL_ERROR,
                                 KWABI_ERR_BODY_RAISED, "operator failed");
        shim_raise_kwabi_error(&err);
    }

    if (is_bool)
        PG_RETURN_BOOL(result != 0);
    if (is_order)
        PG_RETURN_INT32((int32) result - 1);    /* less, equal, greater -> -1, 0, 1 */
    PG_RETURN_UINT64(result);
}
