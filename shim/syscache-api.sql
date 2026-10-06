-- ========================================================================
-- syscache-api.sql — the syscache group through the ABI.
--
-- This is the test for the syscache slots. It proves an extension can
-- look up system catalog entries through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: the negative control raises by design.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_syscache_control() asserts oid == 999 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as relation-api.sql and fmgr-api.sql.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

-- Register the SQL-callable wrappers.
DROP FUNCTION IF EXISTS kwabi_syscache_test();
DROP FUNCTION IF EXISTS kwabi_syscache_control();

CREATE FUNCTION kwabi_syscache_test()
    RETURNS text AS :'bundle','kwabi_syscache_test' LANGUAGE C;
CREATE FUNCTION kwabi_syscache_control()
    RETURNS bool AS :'bundle','kwabi_syscache_control' LANGUAGE C;

\echo ''
\echo '=== 1. syscache get_oid/get_tuple/free_tuple ==='
SELECT kwabi_syscache_test() = 'kwabi: syscache test passed' AS syscache_basic;

\echo ''
\echo '=== 2. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_syscache_control() AS control_should_not_return;

\echo ''
\echo '=== 3. the backend survived all of the above ==='
SELECT kwabi_syscache_test() = 'kwabi: syscache test passed' AS after_all_checks;

\echo ''
\echo '=== syscache-api tests complete ==='
