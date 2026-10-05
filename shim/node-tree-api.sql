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
-- ON_ERROR_STOP is off: check 8 raises by design (the negative control).
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
DROP FUNCTION IF EXISTS kwabi_query_has_for_update(text);
DROP FUNCTION IF EXISTS kwabi_query_has_row_security(text);
DROP FUNCTION IF EXISTS kwabi_planned_stmt_is_utility(text);
DROP FUNCTION IF EXISTS kwabi_node_control();

CREATE FUNCTION kwabi_node_type(sql text)
    RETURNS int4 AS :'bundle','kwabi_node_type' LANGUAGE C;
CREATE FUNCTION kwabi_node_type_name(sql text)
    RETURNS text AS :'bundle','kwabi_node_type_name' LANGUAGE C;
CREATE FUNCTION kwabi_node_list_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_node_list_length' LANGUAGE C;
CREATE FUNCTION kwabi_node_list_get(sql text, int4 idx)
    RETURNS int4 AS :'bundle','kwabi_node_list_get' LANGUAGE C;
CREATE FUNCTION kwabi_query_command_type(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_command_type' LANGUAGE C;
CREATE FUNCTION kwabi_query_rtable_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_rtable_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_target_list_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_target_list_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_returning_list_length(sql text)
    RETURNS int4 AS :'bundle','kwabi_query_returning_list_length' LANGUAGE C;
CREATE FUNCTION kwabi_query_has_for_update(sql text)
    RETURNS bool AS :'bundle','kwabi_query_has_for_update' LANGUAGE C;
CREATE FUNCTION kwabi_query_has_row_security(sql text)
    RETURNS bool AS :'bundle','kwabi_query_has_row_security' LANGUAGE C;
CREATE FUNCTION kwabi_planned_stmt_is_utility(sql text)
    RETURNS bool AS :'bundle','kwabi_planned_stmt_is_utility' LANGUAGE C;
CREATE FUNCTION kwabi_node_control()
    RETURNS bool AS :'bundle','kwabi_node_control' LANGUAGE C;

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
\echo '=== 9. query has for update ==='
\echo '   SELECT 1 must NOT have FOR UPDATE'
SELECT kwabi_query_has_for_update('SELECT 1') = false AS no_for_update;

\echo ''
\echo '=== 10. query has row security ==='
\echo '   SELECT 1 must NOT have row security'
SELECT kwabi_query_has_row_security('SELECT 1') = false AS no_row_security;

\echo ''
\echo '=== 11. planned stmt is utility ==='
\echo '   SELECT 1 must NOT be a utility statement'
SELECT kwabi_planned_stmt_is_utility('SELECT 1') = false AS not_utility;

\echo ''
\echo '=== 12. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_node_control() AS control_should_not_return;

\echo ''
\echo '=== 13. the backend survived all of the above ==='
SELECT kwabi_node_type('SELECT 42') = 1 AS after_control;

\echo ''
\echo '=== node-tree-api tests complete ==='
