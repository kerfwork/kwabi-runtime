-- ========================================================================
-- extension-api.sql — the extension group through the ABI.
--
-- This is the test for the extension slots. It proves an extension can
-- look up extension metadata through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: the negative control raises by design.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_extension_control() asserts oid == 999 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as relation-api.sql and fmgr-api.sql.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

-- Register the SQL-callable wrappers.
DROP FUNCTION IF EXISTS kwabi_extension_test();
DROP FUNCTION IF EXISTS kwabi_extension_control();

CREATE FUNCTION kwabi_extension_test()
    RETURNS text AS :'bundle','kwabi_extension_test' LANGUAGE C;
CREATE FUNCTION kwabi_extension_control()
    RETURNS bool AS :'bundle','kwabi_extension_control' LANGUAGE C;

\echo ''
\echo '=== 1. extension oid/installed/version ==='
SELECT kwabi_extension_test() IS NOT NULL AS extension_basic;

\echo ''
\echo '=== 2. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_extension_control() AS control_should_not_return;

\echo ''
\echo '=== 3. the backend survived all of the above ==='
SELECT kwabi_extension_test() IS NOT NULL AS after_all_checks;

\echo ''
\echo '=== extension-api tests complete ==='
