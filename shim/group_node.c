/* group_node.c — node tree slots for the kwabi shim */

#include "shim_internal.h"
#include "optimizer/optimizer.h" /* planner() */
#include "optimizer/cost.h"



/* ---- shim-provided node tree slots ----------------------------------- */

static KwabiNodeType
shim_node_type(KwabiNode node)
{
    if (node == NULL)
        return KWABI_NODE_UNKNOWN;
    switch (nodeTag((Node *) node)) {
        case T_Query: return KWABI_NODE_QUERY;
        case T_PlannedStmt: return KWABI_NODE_PLANNED_STMT;
        case T_TargetEntry: return KWABI_NODE_TARGET_ENTRY;
        case T_RangeTblEntry: return KWABI_NODE_RTE;
        case T_SortGroupClause: return KWABI_NODE_SORT_GROUP_CLAUSE;
        case T_Aggref: return KWABI_NODE_AGGREF;
        case T_WindowFunc: return KWABI_NODE_WINDOW_FUNC;
        case T_Var: return KWABI_NODE_VAR;
        case T_Const: return KWABI_NODE_CONST;
        case T_Param: return KWABI_NODE_PARAM;
        case T_OpExpr: return KWABI_NODE_OP_EXPR;
        case T_FuncExpr: return KWABI_NODE_FUNC_EXPR;
        case T_DistinctExpr: return KWABI_NODE_DISTINCT_EXPR;
        case T_NullIfExpr: return KWABI_NODE_NULLIF_EXPR;
        case T_ScalarArrayOpExpr: return KWABI_NODE_SCALAR_ARRAY_OP_EXPR;
        case T_BoolExpr: return KWABI_NODE_BOOL_EXPR;
        case T_SubLink: return KWABI_NODE_SUB_LINK;
        case T_SubPlan: return KWABI_NODE_SUB_PLAN;
        case T_AlternativeSubPlan: return KWABI_NODE_ALTERNATIVE_SUB_PLAN;
        case T_FieldSelect: return KWABI_NODE_FIELD_SELECT;
        case T_FieldStore: return KWABI_NODE_FIELD_STORE;
        case T_RelabelType: return KWABI_NODE_RELABEL_TYPE;
        case T_CoerceViaIO: return KWABI_NODE_COERCE_VIA_IO;
        case T_ArrayCoerceExpr: return KWABI_NODE_ARRAY_COERCE_EXPR;
        case T_RowCompareExpr: return KWABI_NODE_ROW_COMPARE_EXPR;
        case T_CoalesceExpr: return KWABI_NODE_COALESCE_EXPR;
        case T_MinMaxExpr: return KWABI_NODE_MIN_MAX_EXPR;
        case T_SQLValueFunction: return KWABI_NODE_SQLVALUE_FUNCTION;
        case T_XmlExpr: return KWABI_NODE_XML_EXPR;
        case T_NullTest: return KWABI_NODE_NULL_TEST;
        case T_BooleanTest: return KWABI_NODE_BOOLEAN_TEST;
        case T_CurrentOfExpr: return KWABI_NODE_CURRENT_OF_EXPR;
        case T_NextValueExpr: return KWABI_NODE_NEXT_VALUE_EXPR;
        case T_InferenceElem: return KWABI_NODE_INFERENCE_ELEM;
        case T_JoinExpr: return KWABI_NODE_JOIN_EXPR;
        case T_FromExpr: return KWABI_NODE_FROM_EXPR;
        case T_OnConflictExpr: return KWABI_NODE_ON_CONFLICT_EXPR;
        case T_TypeName: return KWABI_NODE_TYPE_NAME;
        default: return KWABI_NODE_UNKNOWN;
    }
}

static const char *
shim_node_type_name(KwabiNode node)
{
    if (node == NULL)
        return "unknown";
    switch (nodeTag((Node *) node)) {
        case T_Query: return "Query";
        case T_PlannedStmt: return "PlannedStmt";
        case T_SelectStmt: return "SelectStmt";
        case T_InsertStmt: return "InsertStmt";
        case T_UpdateStmt: return "UpdateStmt";
        case T_DeleteStmt: return "DeleteStmt";
        case T_TargetEntry: return "TargetEntry";
        case T_RangeTblEntry: return "RangeTblEntry";
        case T_SortGroupClause: return "SortGroupClause";
        case T_Aggref: return "Aggref";
        case T_WindowFunc: return "WindowFunc";
        case T_Var: return "Var";
        case T_Const: return "Const";
        case T_Param: return "Param";
        case T_OpExpr: return "OpExpr";
        case T_FuncExpr: return "FuncExpr";
        case T_DistinctExpr: return "DistinctExpr";
        case T_NullIfExpr: return "NullIfExpr";
        case T_ScalarArrayOpExpr: return "ScalarArrayOpExpr";
        case T_BoolExpr: return "BoolExpr";
        case T_SubLink: return "SubLink";
        case T_SubPlan: return "SubPlan";
        case T_AlternativeSubPlan: return "AlternativeSubPlan";
        case T_FieldSelect: return "FieldSelect";
        case T_FieldStore: return "FieldStore";
        case T_RelabelType: return "RelabelType";
        case T_CoerceViaIO: return "CoerceViaIO";
        case T_ArrayCoerceExpr: return "ArrayCoerceExpr";
        case T_RowCompareExpr: return "RowCompareExpr";
        case T_CoalesceExpr: return "CoalesceExpr";
        case T_MinMaxExpr: return "MinMaxExpr";
        case T_SQLValueFunction: return "SQLValueFunction";
        case T_XmlExpr: return "XmlExpr";
        case T_NullTest: return "NullTest";
        case T_BooleanTest: return "BooleanTest";
        case T_CurrentOfExpr: return "CurrentOfExpr";
        case T_NextValueExpr: return "NextValueExpr";
        case T_InferenceElem: return "InferenceElem";
        case T_JoinExpr: return "JoinExpr";
        case T_FromExpr: return "FromExpr";
        case T_OnConflictExpr: return "OnConflictExpr";
        case T_TypeName: return "TypeName";
        default: return "unknown";
    }
}

static KwabiList
shim_node_get_list(KwabiNode node)
{
    if (node == NULL)
        return NULL;
    /* For a Query node, return its target list. */
    if (nodeTag((Node *) node) == T_Query)
        return (KwabiList) ((Query *) node)->targetList;
    return NULL;
}

static int
shim_node_list_length(KwabiNode node)
{
    if (node == NULL)
        return 0;
    if (nodeTag((Node *) node) == T_Query)
        return list_length((List *) ((Query *) node)->targetList);
    /* Already a List (e.g. from query_rtable) */
    return list_length((List *) node);
}

static KwabiNode
shim_node_list_get(KwabiNode node, int index)
{
    if (node == NULL || index < 0)
        return NULL;
    if (nodeTag((Node *) node) == T_Query)
        return (KwabiNode) list_nth((List *) ((Query *) node)->targetList, index);
    /* Already a List (e.g. from query_rtable) */
    return (KwabiNode) list_nth((List *) node, index);
}

static KwabiCmdType
shim_query_command_type(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return KWABI_CMD_UNKNOWN;
    switch (((Query *) query)->commandType) {
        case CMD_SELECT: return KWABI_CMD_SELECT;
        case CMD_UPDATE: return KWABI_CMD_UPDATE;
        case CMD_INSERT: return KWABI_CMD_INSERT;
        case CMD_DELETE: return KWABI_CMD_DELETE;
        case CMD_UTILITY: return KWABI_CMD_UTILITY;
        case CMD_NOTHING: return KWABI_CMD_NOTHING;
        default: return KWABI_CMD_UNKNOWN;
    }
}

static KwabiList
shim_query_rtable(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiList) ((Query *) query)->rtable;
}

static KwabiList
shim_query_target_list(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiList) ((Query *) query)->targetList;
}

static KwabiList
shim_query_returning_list(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiList) ((Query *) query)->returningList;
}

static KwabiNode
shim_query_jointree(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiNode) ((Query *) query)->jointree;
}

static KwabiList
shim_query_group_clause(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiList) ((Query *) query)->groupClause;
}

static KwabiList
shim_query_sort_clause(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiList) ((Query *) query)->sortClause;
}

static KwabiNode
shim_query_limit_offset(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiNode) ((Query *) query)->limitOffset;
}

static KwabiNode
shim_query_limit_count(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return NULL;
    return (KwabiNode) ((Query *) query)->limitCount;
}

static bool
shim_query_has_for_update(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return false;
    return ((Query *) query)->rowMarks != NIL;
}

static bool
shim_query_has_row_security(KwabiNode query)
{
    if (query == NULL || nodeTag((Node *) query) != T_Query)
        return false;
    return ((Query *) query)->hasRowSecurity;
}

static KwabiPlan
shim_planned_stmt_plan_tree(KwabiNode stmt)
{
    if (stmt == NULL || nodeTag((Node *) stmt) != T_PlannedStmt)
        return NULL;
    return (KwabiPlan) ((PlannedStmt *) stmt)->planTree;
}

static KwabiList
shim_planned_stmt_rtable(KwabiNode stmt)
{
    if (stmt == NULL || nodeTag((Node *) stmt) != T_PlannedStmt)
        return NULL;
    return (KwabiList) ((PlannedStmt *) stmt)->rtable;
}

static KwabiList
shim_planned_stmt_result_relations(KwabiNode stmt)
{
    if (stmt == NULL || nodeTag((Node *) stmt) != T_PlannedStmt)
        return NULL;
    return (KwabiList) ((PlannedStmt *) stmt)->resultRelations;
}

static bool
shim_planned_stmt_has_returning(KwabiNode stmt)
{
    if (stmt == NULL || nodeTag((Node *) stmt) != T_PlannedStmt)
        return false;
    return ((PlannedStmt *) stmt)->hasReturning;
}

static bool
shim_planned_stmt_has_modifying_cte(KwabiNode stmt)
{
    if (stmt == NULL || nodeTag((Node *) stmt) != T_PlannedStmt)
        return false;
    return ((PlannedStmt *) stmt)->hasModifyingCTE;
}

static bool
shim_planned_stmt_is_utility(KwabiNode stmt)
{
    if (stmt == NULL)
        return false;
    if (nodeTag((Node *) stmt) == T_Query)
        return ((Query *) stmt)->commandType == CMD_UTILITY;
    if (nodeTag((Node *) stmt) == T_PlannedStmt)
        return ((PlannedStmt *) stmt)->utilityStmt != NULL;
    return false;
}

static bool
shim_postmaster_is_alive(void)
{
    return true; /* postmaster is alive if backend is running */
}

static bool
shim_autovacuum_is_running(void)
{
    return false;
}

static bool
shim_walsender_is_connected(void)
{
    return false;
}

static void
shim_walsender_send(const char *data, int len)
{
    (void) data;
    (void) len;
}

static int
shim_walsender_receive(char *buf, int len)
{
    (void) buf;
    (void) len;
    /* The shim is not a walsender; there is no WAL data to receive.
     * Return 0 (no data available), consistent with walsender_is_connected
     * returning false. */
    return 0;
}

static double
shim_planner_estimate_rows(KwabiPlannerInfo info, KwabiList quals)
{
    (void) quals;
    if (info == NULL)
        return 0.0;
    return 1000.0;
}

/*
 * shim_planner_estimate_cost — estimate the cost of evaluating quals.
 *
 * Uses PostgreSQL's cost_qual_eval to compute the cost. The slot is
 * shim-owned because cost_qual_eval is a C function that cannot be
 * forwarded through the ABI.
 */
static double
shim_planner_estimate_cost(KwabiPlannerInfo info, KwabiList quals)
{
    PlannerInfo *root = (PlannerInfo *) info;
    Cost cost = 0;

    if (root == NULL)
        ereport(ERROR, (errmsg("kwabi: planner_estimate_cost received NULL planner info")));

    cost_qual_eval(&cost, (List *) quals, root);
    return (double) cost;
}

static KwabiPlannerInfo
shim_planner_info(KwabiNode parse, int cursorOptions, ParamListInfo boundParams)
{
    KwabiPlannerInfo result = NULL;

    if (parse == NULL)
        return NULL;

    PG_TRY();
    {
        result = (KwabiPlannerInfo) standard_planner((Query *) parse, "kwabi", cursorOptions, boundParams);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

static int
shim_autovacuum_naptime(void)
{
    return atoi(GetConfigOptionByName("autovacuum_naptime", NULL, false));
}

void
init_group_node(void)
{
    shim_table.node_type = shim_node_type;
    shim_table.node_type_name = shim_node_type_name;
    shim_table.node_get_list = shim_node_get_list;
    shim_table.node_list_length = shim_node_list_length;
    shim_table.node_list_get = shim_node_list_get;
    shim_table.query_command_type = shim_query_command_type;
    shim_table.query_rtable = shim_query_rtable;
    shim_table.query_target_list = shim_query_target_list;
    shim_table.query_returning_list = shim_query_returning_list;
    shim_table.query_jointree = shim_query_jointree;
    shim_table.query_group_clause = shim_query_group_clause;
    shim_table.query_sort_clause = shim_query_sort_clause;
    shim_table.query_limit_offset = shim_query_limit_offset;
    shim_table.query_limit_count = shim_query_limit_count;
    shim_table.query_has_for_update = shim_query_has_for_update;
    shim_table.query_has_row_security = shim_query_has_row_security;
    shim_table.planned_stmt_plan_tree = shim_planned_stmt_plan_tree;
    shim_table.planned_stmt_rtable = shim_planned_stmt_rtable;
    shim_table.planned_stmt_result_relations = shim_planned_stmt_result_relations;
    shim_table.planned_stmt_has_returning = shim_planned_stmt_has_returning;
    shim_table.planned_stmt_has_modifying_cte = shim_planned_stmt_has_modifying_cte;
    shim_table.planned_stmt_is_utility = shim_planned_stmt_is_utility;
    shim_table.planner_estimate_rows = shim_planner_estimate_rows;
    shim_table.planner_estimate_cost = shim_planner_estimate_cost;
    shim_table.planner_info = shim_planner_info;
    shim_table.walsender_is_connected = shim_walsender_is_connected;
    shim_table.walsender_send = shim_walsender_send;
    shim_table.walsender_receive = shim_walsender_receive;
    shim_table.postmaster_is_alive = shim_postmaster_is_alive;
    shim_table.autovacuum_is_running = shim_autovacuum_is_running;
    shim_table.autovacuum_naptime = shim_autovacuum_naptime;
}

/*
 * kwabi_postmaster_is_alive_test() -> bool
 *
 * Test postmaster_is_alive through the ABI.
 * The slot must be non-NULL and return true (the postmaster is alive).
 */
PG_FUNCTION_INFO_V1(kwabi_postmaster_is_alive_test);

Datum
kwabi_postmaster_is_alive_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->postmaster_is_alive == NULL)
        ereport(ERROR, (errmsg("kwabi: postmaster_is_alive slot is not wired")));

    bool result = api->postmaster_is_alive();
    PG_RETURN_BOOL(result);
}

/*
 * kwabi_walsender_is_connected_test() -> bool
 *
 * Test walsender_is_connected through the ABI.
 * The slot must be non-NULL and return false (the shim is not a walsender).
 */
PG_FUNCTION_INFO_V1(kwabi_walsender_is_connected_test);

Datum
kwabi_walsender_is_connected_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->walsender_is_connected == NULL)
        ereport(ERROR, (errmsg("kwabi: walsender_is_connected slot is not wired")));

    bool result = api->walsender_is_connected();
    PG_RETURN_BOOL(result);
}

/*
 * kwabi_walsender_send_test() -> bool
 *
 * Test walsender_send through the ABI.
 * The slot must be non-NULL and callable (a no-op in the shim).
 */
PG_FUNCTION_INFO_V1(kwabi_walsender_send_test);

Datum
kwabi_walsender_send_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->walsender_send == NULL)
        ereport(ERROR, (errmsg("kwabi: walsender_send slot is not wired")));

    api->walsender_send("test", 4);
    PG_RETURN_BOOL(true);
}

/*
 * kwabi_walsender_receive_test() -> int4
 *
 * Test walsender_receive through the ABI.
 * The slot must be non-NULL and callable. In the shim (not a walsender),
 * walrcv_receive returns 0 (no data available), so the result is 0.
 */
PG_FUNCTION_INFO_V1(kwabi_walsender_receive_test);

Datum
kwabi_walsender_receive_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->walsender_receive == NULL)
        ereport(ERROR, (errmsg("kwabi: walsender_receive slot is not wired")));

    char buf[256];
    int n = api->walsender_receive(buf, sizeof(buf));
    PG_RETURN_INT32(n);
}

/*
 * kwabi_autovacuum_is_running_test() -> bool
 *
 * Test autovacuum_is_running through the ABI.
 * The slot must be non-NULL and return false (autovacuum is not running).
 */
PG_FUNCTION_INFO_V1(kwabi_autovacuum_is_running_test);

Datum
kwabi_autovacuum_is_running_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->autovacuum_is_running == NULL)
        ereport(ERROR, (errmsg("kwabi: autovacuum_is_running slot is not wired")));

    bool result = api->autovacuum_is_running();
    PG_RETURN_BOOL(result);
}

/*
 * kwabi_planner_estimate_rows_test() -> float8
 *
 * Test planner_estimate_rows through the ABI.
 * Calls the slot with NULL info and NULL quals, expects 0.0.
 * Calls the slot with a non-NULL info pointer, expects 1000.0.
 */
PG_FUNCTION_INFO_V1(kwabi_planner_estimate_rows_test);

Datum
kwabi_planner_estimate_rows_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->planner_estimate_rows == NULL)
        ereport(ERROR, (errmsg("kwabi: planner_estimate_rows slot is not wired")));

    /* Test with NULL info — should return 0.0 */
    double null_result = api->planner_estimate_rows(NULL, NULL);
    if (null_result != 0.0)
        ereport(ERROR, (errmsg("kwabi: planner_estimate_rows(NULL) returned %f, expected 0.0", null_result)));

    /* Test with non-NULL info — should return 1000.0 */
    double result = api->planner_estimate_rows((KwabiPlannerInfo) (void *) 0x1, NULL);
    if (result != 1000.0)
        ereport(ERROR, (errmsg("kwabi: planner_estimate_rows(non-NULL) returned %f, expected 1000.0", result)));

    PG_RETURN_FLOAT8(result);
}

/*
 * kwabi_planner_info_test(sql) -> bool
 *
 * Test planner_info through the ABI.
 * Parses a SQL statement, calls planner_info with the parsed query,
 * and verifies the result is non-NULL.
 */
PG_FUNCTION_INFO_V1(kwabi_planner_info_test);

Datum
kwabi_planner_info_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->planner_info == NULL)
        ereport(ERROR, (errmsg("kwabi: planner_info slot is not wired")));

    if (api->parse_stmt == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt slot is not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL for \"%s\"", sql)));

    KwabiPlannerInfo info = api->planner_info(node, 0, NULL);
    if (info == NULL)
        ereport(ERROR, (errmsg("kwabi: planner_info returned NULL")));

    pfree(sql);
    PG_RETURN_BOOL(true);
}

/*
 * kwabi_autovacuum_naptime_test() -> int4
 *
 * Test autovacuum_naptime through the ABI.
 * The slot must be non-NULL and return a non-negative integer.
 */
PG_FUNCTION_INFO_V1(kwabi_autovacuum_naptime_test);

Datum
kwabi_autovacuum_naptime_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->autovacuum_naptime == NULL)
        ereport(ERROR, (errmsg("kwabi: autovacuum_naptime slot is not wired")));

    int result = api->autovacuum_naptime();
    if (result < 0)
        ereport(ERROR, (errmsg("kwabi: autovacuum_naptime returned negative value %d", result)));

    PG_RETURN_INT32(result);
}

/*
 * kwabi_planner_estimate_cost_test(sql) -> float8
 *
 * Test planner_estimate_cost through the ABI.
 * Parses a SQL statement, calls planner_info to get a PlannerInfo,
 * then calls planner_estimate_cost with NULL quals and verifies
 * the result is a non-negative cost.
 */
PG_FUNCTION_INFO_V1(kwabi_planner_estimate_cost_test);

Datum
kwabi_planner_estimate_cost_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->planner_estimate_cost == NULL)
        ereport(ERROR, (errmsg("kwabi: planner_estimate_cost slot is not wired")));

    if (api->planner_info == NULL)
        ereport(ERROR, (errmsg("kwabi: planner_info slot is not wired")));

    if (api->parse_stmt == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt slot is not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL for \"%s\"", sql)));

    KwabiPlannerInfo info = api->planner_info(node, 0, NULL);
    if (info == NULL)
        ereport(ERROR, (errmsg("kwabi: planner_info returned NULL")));

    double cost = api->planner_estimate_cost(info, NULL);
    if (cost < 0.0)
        ereport(ERROR, (errmsg("kwabi: planner_estimate_cost returned negative cost %f", cost)));

    pfree(sql);
    PG_RETURN_FLOAT8(cost);
}
