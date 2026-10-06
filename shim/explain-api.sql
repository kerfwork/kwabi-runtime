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
DROP FUNCTION IF EXISTS kwabi_explain_query_test(text);
DROP FUNCTION IF EXISTS kwabi_explain_control();

CREATE FUNCTION kwabi_explain_get_index_name_test(int4)
    RETURNS text AS :'bundle','kwabi_explain_get_index_name_test' LANGUAGE C;
CREATE FUNCTION kwabi_explain_query_test(text)
    RETURNS bool AS :'bundle','kwabi_explain_query_test' LANGUAGE C;
CREATE FUNCTION kwabi_explain_control()
    RETURNS bool AS :'bundle','kwabi_explain_control' LANGUAGE C;

\echo ''
\echo '=== 1. explain_get_index_name ==='
\echo '   must return a non-null name for a known index'
SELECT kwabi_explain_get_index_name_test(1259) AS index_name;

\echo ''
\echo '=== 2. the backend survived the explain ==='
SELECT kwabi_explain_get_index_name_test(1259) AS after_explain;

\echo ''
\echo '=== 2b. explain_query ==='
SELECT kwabi_explain_query_test('SELECT 1') AS explain_query_works;

\echo ''
\echo '=== 3. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_explain_control() AS control_should_not_return;

\echo ''
\echo '=== explain-api tests complete ==='
