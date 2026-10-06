-- ========================================================================
-- executor-api.sql — the executor group through the ABI.
--
-- This is the test for the executor slots. It proves an extension can
-- start the executor, run it, get results, and finish — entirely through
-- the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 3 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_executor_control() asserts 1 == 2 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_executor_test();
DROP FUNCTION IF EXISTS kwabi_executor_control();

CREATE FUNCTION kwabi_executor_test()
    RETURNS text AS :'bundle','kwabi_executor_test' LANGUAGE C;
CREATE FUNCTION kwabi_executor_control()
    RETURNS bool AS :'bundle','kwabi_executor_control' LANGUAGE C;

\echo ''
\echo '=== 1. executor_start + executor_run + executor_getnext + executor_finish + executor_end ==='
\echo '   must start, run, get 3 rows, and finish cleanly'
SELECT kwabi_executor_test() AS executor_lifecycle;

\echo ''
\echo '=== 2. the backend survived the executor ==='
SELECT kwabi_executor_test() AS after_executor;

\echo ''
\echo '=== 3. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_executor_control() AS control_should_not_return;

\echo ''
\echo '=== executor-api tests complete ==='
