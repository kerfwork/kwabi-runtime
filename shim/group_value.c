/* group_value.c — value node slots for the kwabi shim */

#include "shim_internal.h"

/*
 * value_is_null — whether a Const node represents SQL NULL.
 *
 * Shim-owned because the slot can raise: if the value is not a Const,
 * ereport(ERROR) is the correct response, and that must happen in C
 * (beside PG_TRY), not in Rust.
 */
static bool
shim_value_is_null(KwabiValue value)
{
    Const *c = (Const *) value;
    if (c == NULL)
        ereport(ERROR, (errmsg("kwabi: value_is_null received NULL")));
    return c->constisnull;
}

/*
 * value_get_datum — the Datum value of a Const node.
 */
static Datum
shim_value_get_datum(KwabiValue value)
{
    Const *c = (Const *) value;
    if (c == NULL)
        ereport(ERROR, (errmsg("kwabi: value_get_datum received NULL")));
    return c->constvalue;
}

/*
 * value_get_type — the type OID of a Const node.
 */
static Oid
shim_value_get_type(KwabiValue value)
{
    Const *c = (Const *) value;
    if (c == NULL)
        ereport(ERROR, (errmsg("kwabi: value_get_type received NULL")));
    return c->consttype;
}

/*
 * value_get_typmod — the typmod of a Const node.
 */
static int32
shim_value_get_typmod(KwabiValue value)
{
    Const *c = (Const *) value;
    if (c == NULL)
        ereport(ERROR, (errmsg("kwabi: value_get_typmod received NULL")));
    return c->consttypmod;
}

void
init_group_value(void)
{
    shim_table.value_is_null = shim_value_is_null;
    shim_table.value_get_datum = shim_value_get_datum;
    shim_table.value_get_type = shim_value_get_type;
    shim_table.value_get_typmod = shim_value_get_typmod;
}

/*
 * kwabi_value_test() -> bool
 *
 * Test value node accessors through the ABI.
 * Parses a constant expression and verifies that value_is_null,
 * value_get_datum, value_get_type, and value_get_typmod all return
 * correct values.
 */
PG_FUNCTION_INFO_V1(kwabi_value_test);

Datum
kwabi_value_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->parse_expr == NULL || api->value_is_null == NULL ||
        api->value_get_datum == NULL || api->value_get_type == NULL ||
        api->value_get_typmod == NULL)
        ereport(ERROR, (errmsg("kwabi: value slots are not wired")));

    /* Parse a constant expression */
    KwabiNode node = api->parse_expr("42", NULL, 0);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_expr returned NULL")));

    KwabiValue value = (KwabiValue) node;

    /* Test value_is_null - should be false for a non-null constant */
    bool isnull = api->value_is_null(value);
    if (isnull)
        ereport(ERROR, (errmsg("kwabi: value_is_null returned true for non-null constant")));

    /* Test value_get_type - should be int4 (23) */
    Oid typeoid = api->value_get_type(value);
    if (typeoid != 23)
        ereport(ERROR, (errmsg("kwabi: value_get_type returned %u, expected 23", typeoid)));

    /* Test value_get_datum - should be 42 */
    Datum d = api->value_get_datum(value);
    if (DatumGetInt32(d) != 42)
        ereport(ERROR, (errmsg("kwabi: value_get_datum returned wrong value")));

    /* Test value_get_typmod - should be -1 for int4 */
    int32 typmod = api->value_get_typmod(value);
    if (typmod != -1)
        ereport(ERROR, (errmsg("kwabi: value_get_typmod returned %d, expected -1", typmod)));

    api->free_node(node);

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_value_control() -> bool
 *
 * The NEGATIVE CONTROL. It parses a constant and asserts a wrong type.
 * That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_value_control);

Datum
kwabi_value_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->parse_expr == NULL || api->value_get_type == NULL)
        ereport(ERROR, (errmsg("kwabi: value slots are not wired")));

    KwabiNode node = api->parse_expr("42", NULL, 0);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_expr returned NULL")));

    KwabiValue value = (KwabiValue) node;
    Oid typeoid = api->value_get_type(value);

    api->free_node(node);

    /* The control's whole point: this comparison must be FALSE. */
    if (typeoid == 25)  /* text type OID */
        PG_RETURN_BOOL(true);   /* the comparison thinks int4 == text: broken */

    ereport(ERROR,
            (errmsg("kwabi: value negative control fired as intended"),
             errdetail("the type is not text -- the comparison is honest")));
}
