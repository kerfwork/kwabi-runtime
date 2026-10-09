-- ========================================================================
-- explain-api.sql — the explain group through the ABI.
--
-- This is the test for the explain slots. It proves an extension can
-- get an index name through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 3 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_explain_control() asserts the index name is wrong and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_explain_get_index_name_test(int4);
DROP FUNCTION IF EXISTS kwabi_explain_control();
DROP FUNCTION IF EXISTS kwabi_explain_matches_sql_test(text);
DROP FUNCTION IF EXISTS kwabi_explain_bad_option_test();
DROP FUNCTION IF EXISTS kwabi_explain_analyze_matches_sql_test(text);
DROP FUNCTION IF EXISTS kwabi_explain_option_version_test();

CREATE FUNCTION kwabi_explain_get_index_name_test(int4)
    RETURNS text AS :'bundle','kwabi_explain_get_index_name_test' LANGUAGE C;
CREATE FUNCTION kwabi_explain_control()
    RETURNS bool AS :'bundle','kwabi_explain_control' LANGUAGE C;
CREATE FUNCTION kwabi_explain_matches_sql_test(text)
    RETURNS bool AS :'bundle','kwabi_explain_matches_sql_test' LANGUAGE C;
CREATE FUNCTION kwabi_explain_bad_option_test()
    RETURNS bool AS :'bundle','kwabi_explain_bad_option_test' LANGUAGE C;
CREATE FUNCTION kwabi_explain_analyze_matches_sql_test(text)
    RETURNS bool AS :'bundle','kwabi_explain_analyze_matches_sql_test' LANGUAGE C;
CREATE FUNCTION kwabi_explain_option_version_test()
    RETURNS bool AS :'bundle','kwabi_explain_option_version_test' LANGUAGE C;

\echo ''
\echo '=== 1. explain_get_index_name ==='
\echo '   must return the name of the relation with OID 1259 (pg_class)'
\echo '   NOTE: this must be a BOOLEAN. It was originally a bare text select,'
\echo '   but the harness counts `^ t` lines, so a text result could never'
\echo '   satisfy it and the check failed however correct the shim was.'
SELECT kwabi_explain_get_index_name_test(1259) = 'pg_class' AS index_name;

\echo ''
\echo '=== 2. the backend survived the explain ==='
SELECT kwabi_explain_get_index_name_test(1259) = 'pg_class' AS after_explain;

\echo ''
\echo '=== 3. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_explain_control() AS control_should_not_return;

\echo ''
\echo ''
-- A private, fixed table for the scan checks. A catalog such as pg_class changes
-- under concurrent sessions and on Linux the planner picks an index-only scan on it,
-- so ABI and SQL row counts drift between two statements. Three rows never do.
DROP TABLE IF EXISTS kwabi_explain_t;
CREATE TABLE kwabi_explain_t AS SELECT g AS v FROM generate_series(1, 3) g;
ANALYZE kwabi_explain_t;

\echo '=== 4. ExplainState through the ABI: EXPLAIN text must equal SQL EXPLAIN (COSTS OFF) ==='
\echo '   the text from explain_query on a shim-owned ExplainState, byte for byte'
SELECT kwabi_explain_matches_sql_test('SELECT 1') AS explain_matches_select;
SELECT kwabi_explain_matches_sql_test('SELECT v FROM kwabi_explain_t') AS explain_matches_scan;

\echo ''
\echo '=== 5. an unknown EXPLAIN option must raise through the ABI ==='
SELECT kwabi_explain_bad_option_test() AS explain_bad_option_raises;

\echo ''
\echo '=== 6. EXPLAIN ANALYZE through the ABI: runs the plan; text must equal SQL EXPLAIN ANALYZE ==='
SELECT kwabi_explain_analyze_matches_sql_test('SELECT v FROM kwabi_explain_t') AS explain_analyze_matches_scan;

\echo ''
\echo '=== 7. version-specific options are accepted only on the majors that have them ==='
SELECT kwabi_explain_option_version_test() AS explain_option_version;

\echo '=== explain-api tests complete ==='
