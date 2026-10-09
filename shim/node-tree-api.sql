-- ========================================================================
-- node-tree-api.sql — the node tree traversal group through the ABI.
--
-- This is the test for the node tree slots. It proves an extension can
-- parse SQL, walk the resulting node tree, and inspect query and plan
-- structures entirely through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 16 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_node_control() asserts 1 == 2 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_node_type(text);
DROP FUNCTION IF EXISTS kwabi_node_type_name(text);
DROP FUNCTION IF EXISTS kwabi_node_list_length(text);
DROP FUNCTION IF EXISTS kwabi_node_list_get(text, int);
DROP FUNCTION IF EXISTS kwabi_query_command_type(text);
DROP FUNCTION IF EXISTS kwabi_query_rtable_length(text);
DROP FUNCTION IF EXISTS kwabi_query_target_list_length(text);
DROP FUNCTION IF EXISTS kwabi_query_returning_list_length(text);
DROP FUNCTION IF EXISTS kwabi_query_sort_clause_length(text);
DROP FUNCTION IF EXISTS kwabi_query_group_clause_length(text);
DROP FUNCTION IF EXISTS kwabi_query_has_for_update(text);
DROP FUNCTION IF EXISTS kwabi_query_has_row_security(text);
DROP FUNCTION IF EXISTS kwabi_query_jointree(text);
DROP FUNCTION IF EXISTS kwabi_planned_stmt_is_utility(text);
DROP FUNCTION IF EXISTS kwabi_planned_stmt_has_modifying_cte(text);
DROP FUNCTION IF EXISTS kwabi_planned_stmt_has_returning(text);
DROP FUNCTION IF EXISTS kwabi_planned_stmt_plan_tree(text);
DROP FUNCTION IF EXISTS kwabi_planned_stmt_result_relations_length(text);
DROP FUNCTION IF EXISTS kwabi_plan_rtable_length(text);
DROP FUNCTION IF EXISTS kwabi_plan_result_relations_length(text);
DROP FUNCTION IF EXISTS kwabi_plan_has_returning(text);
DROP FUNCTION IF EXISTS kwabi_plan_has_modifying_cte(text);
DROP FUNCTION IF EXISTS kwabi_query_limit_count(text);
DROP FUNCTION IF EXISTS kwabi_query_limit_offset(text);
DROP FUNCTION IF EXISTS kwabi_planned_stmt_rtable_length(sql text);
DROP FUNCTION IF EXISTS kwabi_planner_estimate_rows_test();
DROP FUNCTION IF EXISTS kwabi_planner_estimate_cost_test(sql text);
DROP FUNCTION IF EXISTS kwabi_postmaster_is_alive_test();
DROP FUNCTION IF EXISTS kwabi_postmaster_get_child_pid_test();
DROP FUNCTION IF EXISTS kwabi_autovacuum_is_running_test();
DROP FUNCTION IF EXISTS kwabi_walsender_is_connected_test();
DROP FUNCTION IF EXISTS kwabi_node_control();
DROP FUNCTION IF EXISTS kwabi_planner_info_test(text);
DROP FUNCTION IF EXISTS kwabi_free_planner_info_test();
DROP FUNCTION IF EXISTS kwabi_reorderbuffer_get_changes_test();
DROP FUNCTION IF EXISTS kwabi_reorderbuffer_get_lsn_test();
DROP FUNCTION IF EXISTS kwabi_logical_decoding_begin_test();
DROP FUNCTION IF EXISTS kwabi_logical_decoding_end_test();
DROP FUNCTION IF EXISTS kwabi_sequence_nextval_test();
DROP FUNCTION IF EXISTS kwabi_trigger_get_test();
DROP FUNCTION IF EXISTS kwabi_trigger_desc_test();
DROP FUNCTION IF EXISTS kwabi_walsender_receive_test();
DROP FUNCTION IF EXISTS kwabi_walsender_send_test();
DROP FUNCTION IF EXISTS kwabi_autovacuum_naptime_test();
DROP FUNCTION IF EXISTS kwabi_output_plugin_shutdown_test();
DROP FUNCTION IF EXISTS kwabi_output_plugin_startup_test();
DROP FUNCTION IF EXISTS kwabi_vacuum_rel_test();
DROP FUNCTION IF EXISTS kwabi_vacuum_analyze_rel_test();
DROP FUNCTION IF EXISTS kwabi_syslogger_log_test();
DROP FUNCTION IF EXISTS kwabi_sequence_currval_test();
DROP FUNCTION IF EXISTS kwabi_sequence_setval_test();

CREATE FUNCTION kwabi_node_type(sql text)
    RETURNS int4 AS :'bundle','kwabi_node_type' LANGUAGE C;
CREATE FUNCTION kwabi_node_type_name(sql text)
    RETURNS text AS :'bundle','kwabi_node_type_name' LANGUAGE C;
CREATE FUNCTION kwabi_node_list_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_node_list_length' LANGUAGE C;
CREATE FUNCTION kwabi_node_list_get(sql text, idx int4)
    RETURNS int4 AS :'bundle','kwabi_node_list_get' LANGUAGE C;
CREATE FUNCTION kwabi_query_command_type(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_command_type' LANGUAGE C;
CREATE FUNCTION kwabi_query_rtable_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_rtable_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_target_list_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_target_list_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_returning_list_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_returning_list_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_sort_clause_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_sort_clause_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_group_clause_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_group_clause_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_has_for_update(sql text)
    RETURNS bool AS :'bundle','kwabi_query_has_for_update' LANGUAGE C;
CREATE FUNCTION kwabi_query_has_row_security(sql text)
    RETURNS bool AS :'bundle','kwabi_query_has_row_security' LANGUAGE C;
CREATE FUNCTION kwabi_query_jointree(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_jointree' LANGUAGE C;
CREATE FUNCTION kwabi_planned_stmt_is_utility(sql text)
    RETURNS bool AS :'bundle','kwabi_planned_stmt_is_utility' LANGUAGE C;
CREATE FUNCTION kwabi_planned_stmt_has_modifying_cte(sql text)
    RETURNS bool AS :'bundle','kwabi_planned_stmt_has_modifying_cte' LANGUAGE C;
CREATE FUNCTION kwabi_planned_stmt_has_returning(sql text)
    RETURNS bool AS :'bundle','kwabi_planned_stmt_has_returning' LANGUAGE C;
CREATE FUNCTION kwabi_planned_stmt_plan_tree(sql text)
    RETURNS int4 AS :'bundle','kwabi_planned_stmt_plan_tree' LANGUAGE C;
CREATE FUNCTION kwabi_query_limit_count(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_limit_count' LANGUAGE C;
CREATE FUNCTION kwabi_query_limit_offset(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_limit_offset' LANGUAGE C;
CREATE FUNCTION kwabi_planned_stmt_result_relations_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_planned_stmt_result_relations_length' LANGUAGE C;
CREATE FUNCTION kwabi_planned_stmt_rtable_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_planned_stmt_rtable_length' LANGUAGE C;
CREATE FUNCTION kwabi_plan_rtable_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_plan_rtable_length' LANGUAGE C;
CREATE FUNCTION kwabi_plan_result_relations_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_plan_result_relations_length' LANGUAGE C;
CREATE FUNCTION kwabi_plan_has_returning(sql text)
    RETURNS bool AS :'bundle','kwabi_plan_has_returning' LANGUAGE C;
CREATE FUNCTION kwabi_plan_has_modifying_cte(sql text)
    RETURNS bool AS :'bundle','kwabi_plan_has_modifying_cte' LANGUAGE C;
CREATE FUNCTION kwabi_planner_estimate_rows_test()
    RETURNS float8 AS :'bundle','kwabi_planner_estimate_rows_test' LANGUAGE C;
CREATE FUNCTION kwabi_planner_info_test(sql text)
    RETURNS bool AS :'bundle','kwabi_planner_info_test' LANGUAGE C;
CREATE FUNCTION kwabi_free_planner_info_test()
    RETURNS bool AS :'bundle','kwabi_free_planner_info_test' LANGUAGE C;
CREATE FUNCTION kwabi_planner_estimate_cost_test(sql text)
    RETURNS float8 AS :'bundle','kwabi_planner_estimate_cost_test' LANGUAGE C;
CREATE FUNCTION kwabi_postmaster_is_alive_test()
    RETURNS bool AS :'bundle','kwabi_postmaster_is_alive_test' LANGUAGE C;
CREATE FUNCTION kwabi_postmaster_get_child_pid_test()
    RETURNS int4 AS :'bundle','kwabi_postmaster_get_child_pid_test' LANGUAGE C;
CREATE FUNCTION kwabi_autovacuum_is_running_test()
    RETURNS bool AS :'bundle','kwabi_autovacuum_is_running_test' LANGUAGE C;
CREATE FUNCTION kwabi_walsender_is_connected_test()
    RETURNS bool AS :'bundle','kwabi_walsender_is_connected_test' LANGUAGE C;
CREATE FUNCTION kwabi_node_control()
    RETURNS bool AS :'bundle','kwabi_node_control' LANGUAGE C;
CREATE FUNCTION kwabi_reorderbuffer_get_changes_test()
    RETURNS bool AS :'bundle','kwabi_reorderbuffer_get_changes_test' LANGUAGE C;
CREATE FUNCTION kwabi_reorderbuffer_get_lsn_test()
    RETURNS bool AS :'bundle','kwabi_reorderbuffer_get_lsn_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_decoding_begin_test()
    RETURNS bool AS :'bundle','kwabi_logical_decoding_begin_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_decoding_end_test()
    RETURNS bool AS :'bundle','kwabi_logical_decoding_end_test' LANGUAGE C;
CREATE FUNCTION kwabi_sequence_nextval_test()
    RETURNS int8 AS :'bundle','kwabi_sequence_nextval_test' LANGUAGE C;
CREATE FUNCTION kwabi_sequence_currval_test()
    RETURNS int8 AS :'bundle','kwabi_sequence_currval_test' LANGUAGE C;
CREATE FUNCTION kwabi_sequence_setval_test()
    RETURNS int8 AS :'bundle','kwabi_sequence_setval_test' LANGUAGE C;
CREATE FUNCTION kwabi_trigger_get_test()
    RETURNS bool AS :'bundle','kwabi_trigger_get_test' LANGUAGE C;
CREATE FUNCTION kwabi_trigger_desc_test()
    RETURNS bool AS :'bundle','kwabi_trigger_desc_test' LANGUAGE C;
CREATE FUNCTION kwabi_walsender_send_test()
    RETURNS bool AS :'bundle','kwabi_walsender_send_test' LANGUAGE C;
CREATE FUNCTION kwabi_walsender_receive_test()
    RETURNS int4 AS :'bundle','kwabi_walsender_receive_test' LANGUAGE C;
CREATE FUNCTION kwabi_autovacuum_naptime_test()
    RETURNS int4 AS :'bundle','kwabi_autovacuum_naptime_test' LANGUAGE C;
CREATE FUNCTION kwabi_output_plugin_shutdown_test()
    RETURNS bool AS :'bundle','kwabi_output_plugin_shutdown_test' LANGUAGE C;
CREATE FUNCTION kwabi_output_plugin_startup_test()
    RETURNS bool AS :'bundle','kwabi_output_plugin_startup_test' LANGUAGE C;
CREATE FUNCTION kwabi_vacuum_rel_test()
    RETURNS bool AS :'bundle','kwabi_vacuum_rel_test' LANGUAGE C;
CREATE FUNCTION kwabi_vacuum_analyze_rel_test()
    RETURNS bool AS :'bundle','kwabi_vacuum_analyze_rel_test' LANGUAGE C;
CREATE FUNCTION kwabi_syslogger_log_test()
    RETURNS bool AS :'bundle','kwabi_syslogger_log_test' LANGUAGE C;

\echo ''
\echo '=== 1. parse a SELECT and get its node type ==='
\echo '   SELECT 1 must parse to a Query node (nodeTag = 1)'
SELECT kwabi_node_type('SELECT 1') = 1 AS node_type_query;

\echo ''
\echo '=== 2. node type name ==='
\echo '   SELECT 1 must have node type name "Query"'
SELECT kwabi_node_type_name('SELECT 1') = 'Query' AS node_type_name_query;

\echo ''
\echo '=== 3. list length of target list ==='
\echo '   SELECT 1 must have a target list of length 1'
SELECT kwabi_node_list_length('SELECT 1') = 1 AS target_list_length;

\echo ''
\echo '=== 4. list get from target list ==='
\echo '   SELECT 1 must have a target entry at index 0 (KWABI_NODE_TARGET_ENTRY = 4)'
SELECT kwabi_node_list_get('SELECT 1', 0) = 4 AS target_list_get;

\echo ''
\echo '=== 5. query command type ==='
\echo '   SELECT 1 must have command type CMD_SELECT (1)'
SELECT kwabi_query_command_type('SELECT 1') = 1 AS command_type_select;

\echo ''
\echo '=== 6. query rtable length ==='
\echo '   SELECT 1 FROM pg_class must have an rtable of length 1'
SELECT kwabi_query_rtable_length('SELECT 1 FROM pg_class') = 1 AS rtable_length;

\echo ''
\echo '=== 7. query target list length ==='
\echo '   SELECT 1 must have a target list of length 1'
SELECT kwabi_query_target_list_length('SELECT 1') = 1 AS query_target_list_length;

\echo ''
\echo '=== 8. query returning list length ==='
\echo '   SELECT 1 must have an empty returning list'
SELECT kwabi_query_returning_list_length('SELECT 1') = 0 AS returning_list_length;

\echo ''
\echo '=== 8b. query sort clause length ==='
\echo '   SELECT 1 must have an empty sort clause (no ORDER BY)'
SELECT kwabi_query_sort_clause_length('SELECT 1') = 0 AS sort_clause_length_empty;
\echo '   SELECT 1 ORDER BY 1 must have a sort clause of length 1'
SELECT kwabi_query_sort_clause_length('SELECT 1 ORDER BY 1') = 1 AS sort_clause_length_one;

\echo ''
\echo '=== 8c. query group clause length ==='
\echo '   SELECT 1 must have an empty group clause (no GROUP BY)'
SELECT kwabi_query_group_clause_length('SELECT 1') = 0 AS group_clause_length_empty;
\echo '   SELECT 1 GROUP BY 1 must have a group clause of length 1'
SELECT kwabi_query_group_clause_length('SELECT 1 GROUP BY 1') = 1 AS group_clause_length_one;

\echo ''
\echo '=== 9. query has for update ==='
\echo '   SELECT 1 must NOT have FOR UPDATE'
SELECT kwabi_query_has_for_update('SELECT 1') = false AS no_for_update;

\echo ''
\echo '=== 10. query has row security ==='
\echo '   SELECT 1 must NOT have row security'
SELECT kwabi_query_has_row_security('SELECT 1') = false AS no_row_security;

\echo ''
\echo '=== 10b. query jointree ==='
\echo '   SELECT 1 must have a jointree of type FromExpr (KWABI_NODE_FROM_EXPR = 38)'
SELECT kwabi_query_jointree('SELECT 1') = 38 AS jointree_from_expr;

\echo ''
\echo '=== 11. planned stmt is utility ==='
\echo '   SELECT 1 must NOT be a utility statement'
SELECT kwabi_planned_stmt_is_utility('SELECT 1') = false AS not_utility;

\echo ''
\echo '=== 12. planned stmt plan tree ==='
\echo '   SELECT 1 parses to a Query, not a PlannedStmt, so plan tree is NULL (0)'
SELECT kwabi_planned_stmt_plan_tree('SELECT 1') = 0 AS plan_tree_null_for_query;

\echo ''
\echo '=== 12b. query limit count ==='
\echo '   SELECT 1 has no LIMIT, so limit count is NULL (0)'
SELECT kwabi_query_limit_count('SELECT 1') = 0 AS limit_count_null;

\echo ''
\echo '=== 12c. query limit offset ==='
\echo '   SELECT 1 has no OFFSET, so limit offset is NULL (0)'
SELECT kwabi_query_limit_offset('SELECT 1') = 0 AS limit_offset_null;
\echo '   SELECT 1 LIMIT 5 OFFSET 3: the OFFSET is an int4->int8 coercion, so the node is FUNC_EXPR (13), not a bare Const'
SELECT kwabi_query_limit_offset('SELECT 1 LIMIT 5 OFFSET 3') = 13 AS limit_offset_const;

\echo ''
\echo '=== 13. planned stmt result relations length ==='
\echo '   SELECT 1 must have an empty resultRelations list'
SELECT kwabi_planned_stmt_result_relations_length('SELECT 1') = 0 AS result_relations_length;

\echo ''
\echo '=== 13b. planned stmt rtable length ==='
\echo '   SELECT 1 parses to a Query, not a PlannedStmt, so rtable is NULL (0)'
SELECT kwabi_planned_stmt_rtable_length('SELECT 1') = 0 AS rtable_length_for_query;

\echo ''
\echo '=== 14. planned stmt has modifying CTE ==='
\echo '   SELECT 1 must NOT have a modifying CTE'
SELECT kwabi_planned_stmt_has_modifying_cte('SELECT 1') = false AS no_modifying_cte;

\echo ''
\echo '=== 15. planned stmt has returning ==='
\echo '   SELECT 1 parses to a Query, not a PlannedStmt, so has_returning is false'
SELECT kwabi_planned_stmt_has_returning('SELECT 1') = false AS no_returning_for_query;

\echo ''
\echo '=== 15e. planned_stmt positive: a real PlannedStmt from pg_plan_query ==='
\echo '   plans only; the DML is never executed'
SELECT kwabi_plan_rtable_length('SELECT 1 FROM pg_class') = 1 AS plan_rtable_length;
SELECT kwabi_plan_result_relations_length('UPDATE pg_class SET relname = relname WHERE false') = 1 AS plan_result_relations;
SELECT kwabi_plan_result_relations_length('SELECT 1') = 0 AS plan_result_relations_select;
SELECT kwabi_plan_has_returning('UPDATE pg_class SET relname = relname WHERE false RETURNING relname') AS plan_has_returning;
SELECT kwabi_plan_has_returning('UPDATE pg_class SET relname = relname WHERE false') = false AS plan_no_returning;
SELECT kwabi_plan_has_modifying_cte('WITH d AS (DELETE FROM pg_class WHERE false RETURNING 1) SELECT 1') AS plan_modifying_cte;
SELECT kwabi_plan_has_modifying_cte('SELECT 1') = false AS plan_no_modifying_cte;

\echo ''
\echo '=== 15c. planner_estimate_rows ==='
\echo '   planner_estimate_rows(NULL, NULL) must return 0.0'
\echo '   planner_estimate_rows(non-NULL, NULL) must return 1000.0'
SELECT kwabi_planner_estimate_rows_test() = 1000.0::float8 AS planner_estimate_rows;

\echo ''
\echo '=== 15d. planner_info ==='
\echo '   planner_info must return a non-NULL PlannerInfo for a parsed query'
SELECT kwabi_planner_info_test('SELECT 1') = true AS planner_info_nonnull;

\echo ''
\echo '=== 15d2. free_planner_info ==='
\echo '   free_planner_info must be wired and callable (no-op for NULL)'
SELECT kwabi_free_planner_info_test() = true AS free_planner_info;

\echo ''
\echo '=== 15e. planner_estimate_cost ==='
\echo '   planner_estimate_cost must return a non-negative cost for a parsed query'
SELECT kwabi_planner_estimate_cost_test('SELECT 1') >= 0.0 AS planner_estimate_cost;

\echo ''
\echo '=== 15f. walsender_is_connected ==='
\echo '   walsender_is_connected must be wired and return false (not a walsender)'
SELECT kwabi_walsender_is_connected_test() = false AS walsender_is_connected;

\echo ''
\echo '=== 15g. postmaster_is_alive ==='
\echo '   postmaster_is_alive must be wired and return true'
SELECT kwabi_postmaster_is_alive_test() = true AS postmaster_is_alive;

\echo ''
\echo '=== 15g2. postmaster_get_child_pid ==='
\echo '   postmaster_get_child_pid must be wired and return -1 (no children in shim)'
SELECT kwabi_postmaster_get_child_pid_test() = -1 AS postmaster_get_child_pid;

\echo ''
\echo '=== 15h. autovacuum_is_running ==='
\echo '   autovacuum_is_running must be wired and return false'
SELECT kwabi_autovacuum_is_running_test() = false AS autovacuum_is_running;

\echo ''
\echo '=== 16. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_node_control() AS control_should_not_return;

\echo ''
\echo '=== 17. the backend survived all of the above ==='
SELECT kwabi_node_type('SELECT 42') = 1 AS after_control;

\echo ''
\echo '=== 18. reorderbuffer_get_changes ==='
\echo '   reorderbuffer_get_changes must raise "not supported by this shim"'
SELECT kwabi_reorderbuffer_get_changes_test() AS reorderbuffer_get_changes;

\echo ''
\echo '=== 15b. reorderbuffer_get_lsn ==='
\echo '   reorderbuffer_get_lsn must raise "not supported by this shim"'
SELECT kwabi_reorderbuffer_get_lsn_test() AS reorderbuffer_get_lsn;

\echo ''

\echo ''
\echo '=== 19. logical_decoding_begin ==='
\echo '   logical_decoding_begin must raise "not supported by this shim", never return a context'
SELECT kwabi_logical_decoding_begin_test() AS logical_decoding_begin;

\echo ''
\echo '=== 20. logical_decoding_end ==='
\echo '   logical_decoding_end(NULL) must return normally'
SELECT kwabi_logical_decoding_end_test() AS logical_decoding_end;

\echo ''

\echo ''

\echo ''
\echo '=== 21. sequence_nextval ==='
\echo '   sequence_nextval must return 1 for a new sequence'
SELECT kwabi_sequence_nextval_test() = 1 AS sequence_nextval;

\echo ''
\echo '=== 21b. sequence_currval ==='
\echo '   sequence_currval must return the same value as sequence_nextval'
SELECT kwabi_sequence_currval_test() = 1 AS sequence_currval;

\echo ''
\echo '=== 22. sequence_setval ==='
\echo '   sequence_setval must set the sequence value to 42'
SELECT kwabi_sequence_setval_test() = 42 AS sequence_setval;

\echo ''
\echo '=== 23. trigger_get ==='
\echo '   trigger_get must return a valid trigger at index 0'
SELECT kwabi_trigger_get_test() AS trigger_get;

\echo ''
\echo '=== 24. trigger_desc ==='
\echo '   trigger_desc must return a non-NULL TriggerDesc for a table with a trigger'
SELECT kwabi_trigger_desc_test() AS trigger_desc;

\echo ''
\echo '=== 25. walsender_send ==='
\echo '   walsender_send must be callable and not crash'
SELECT kwabi_walsender_send_test() AS walsender_send;

\echo ''
\echo '=== 25b. walsender_receive ==='
\echo '   walsender_receive must be callable and return 0 (no data in shim)'
SELECT kwabi_walsender_receive_test() = 0 AS walsender_receive;

\echo ''
\echo '=== 25c. autovacuum_naptime ==='
\echo '   autovacuum_naptime must be wired and return a non-negative integer'
SELECT kwabi_autovacuum_naptime_test() >= 0 AS autovacuum_naptime;

\echo ''
\echo '=== 25c2. output_plugin_shutdown ==='
\echo '   output_plugin_shutdown must be wired and callable (no-op in shim)'
SELECT kwabi_output_plugin_shutdown_test() = true AS output_plugin_shutdown;

\echo ''
\echo '=== 25c3. output_plugin_startup ==='
\echo '   output_plugin_startup must be wired and callable (no-op in shim)'
SELECT kwabi_output_plugin_startup_test() = true AS output_plugin_startup;

\echo ''
\echo '=== 25d. vacuum_rel ==='
\echo '   vacuum_rel must be wired and callable on a temp table'
SELECT kwabi_vacuum_rel_test() AS vacuum_rel;

\echo ''
\echo '=== 25e. vacuum_analyze_rel ==='
\echo '   vacuum_analyze_rel must be wired and callable on a temp table'
SELECT kwabi_vacuum_analyze_rel_test() AS vacuum_analyze_rel;

\echo ''
\echo '=== 25f. syslogger_log ==='
\echo '   syslogger_log must be wired and callable (no-op in shim)'
SELECT kwabi_syslogger_log_test() = true AS syslogger_log;

\echo ''
\echo '=== node-tree-api tests complete ==='
