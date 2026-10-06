/* group_parser.c — parser slots for the kwabi shim */

#include "shim_internal.h"

static KwabiNode
shim_parse_expr(const char *sql, Oid *argtypes, int nargs)
{
    KwabiNode result = NULL;

    if (sql == NULL)
        return NULL;

    PG_TRY();
    {
        List *tree = raw_parser(sql, RAW_PARSE_PLPGSQL_EXPR);
        RawStmt *raw;
        Query *query;
        Node *expr;

        if (tree == NULL || list_length(tree) != 1) {
            result = NULL;
        } else {
            raw = (RawStmt *) linitial(tree);
            query = parse_analyze_fixedparams(raw, sql, argtypes, nargs, NULL);
            expr = (Node *) ((TargetEntry *) linitial(query->targetList))->expr;
            result = (KwabiNode) expr;
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

static KwabiNode
shim_parse_stmt(const char *sql)
{
    KwabiNode result = NULL;

    if (sql == NULL)
        return NULL;

    PG_TRY();
    {
        List *tree = raw_parser(sql, RAW_PARSE_DEFAULT);
        RawStmt *raw;
        Query *query;

        if (tree == NULL || list_length(tree) != 1) {
            result = NULL;
        } else {
            raw = (RawStmt *) linitial(tree);
            query = parse_analyze_fixedparams(raw, sql, NULL, 0, NULL);
            result = (KwabiNode) query;
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

static KwabiNode
shim_parse_type(const char *type_name)
{
    KwabiNode result = NULL;

    if (type_name == NULL)
        return NULL;

    PG_TRY();
    {
        result = (KwabiNode) typeStringToTypeName(type_name, NULL);
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
shim_free_node(KwabiNode node)
{
    if (node == NULL)
        return;

    PG_TRY();
    {
        pfree(node);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static Oid
shim_oper_left_type(Oid oper_oid)
{
    Oid result = InvalidOid;
    HeapTuple tup;
    bool isnull;

    if (!OidIsValid(oper_oid))
        return InvalidOid;

    PG_TRY();
    {
        tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oper_oid));
        if (HeapTupleIsValid(tup)) {
            result = DatumGetObjectId(SysCacheGetAttr(OPEROID, tup,
                Anum_pg_operator_oprleft, &isnull));
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

static Oid
shim_oper_right_type(Oid oper_oid)
{
    Oid result = InvalidOid;
    HeapTuple tup;
    bool isnull;

    if (!OidIsValid(oper_oid))
        return InvalidOid;

    PG_TRY();
    {
        tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oper_oid));
        if (HeapTupleIsValid(tup)) {
            result = DatumGetObjectId(SysCacheGetAttr(OPEROID, tup,
                Anum_pg_operator_oprright, &isnull));
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

static Oid
shim_oper_result_type(Oid oper_oid)
{
    Oid result = InvalidOid;
    HeapTuple tup;
    bool isnull;

    if (!OidIsValid(oper_oid))
        return InvalidOid;

    PG_TRY();
    {
        tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oper_oid));
        if (HeapTupleIsValid(tup)) {
            result = DatumGetObjectId(SysCacheGetAttr(OPEROID, tup,
                Anum_pg_operator_oprresult, &isnull));
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

static bool
shim_oper_is_commutative(Oid oper_oid)
{
    bool result = false;
    HeapTuple tup;
    bool isnull;
    Oid commutator;

    if (!OidIsValid(oper_oid))
        return false;

    PG_TRY();
    {
        tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oper_oid));
        if (HeapTupleIsValid(tup)) {
            commutator = DatumGetObjectId(SysCacheGetAttr(OPEROID, tup,
                Anum_pg_operator_oprcom, &isnull));
            ReleaseSysCache(tup);
            result = (commutator == oper_oid);
        }
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = false;
    }
    PG_END_TRY();

    return result;
}

void
init_group_parser(void)
{
    shim_table.parse_expr = shim_parse_expr;
    shim_table.parse_stmt = shim_parse_stmt;
    shim_table.parse_type = shim_parse_type;
    shim_table.free_node = shim_free_node;
    shim_table.oper_left_type = shim_oper_left_type;
    shim_table.oper_right_type = shim_oper_right_type;
    shim_table.oper_result_type = shim_oper_result_type;
    shim_table.oper_is_commutative = shim_oper_is_commutative;
}
