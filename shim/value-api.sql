-- ========================================================================
-- value-api.sql — the value node group through the ABI.
--
-- This is the test for the value kata. It proves an extension can
-- inspect Const nodes entirely through the ABI — no direct PostgreSQL
-- calls.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 4 raises by design (the negative control) and
-- the script must survive it.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_value_control() asserts a wrong value and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as mem-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_value_test();
DROP FUNCTION IF EXISTS kwabi_value_control();

CREATE FUNCTION kwabi_value_test()
    RETURNS bool AS :'bundle','kwabi_value_test' LANGUAGE C;
CREATE FUNCTION kwabi_value_control()
    RETURNS bool AS :'bundle','kwabi_value_control' LANGUAGE C;

\echo ''
\echo '=== 1. value node accessors ==='
\echo '   parse a constant, verify value_is_null, value_get_datum,'
\echo '   value_get_type, and value_get_typmod'
SELECT kwabi_value_test() AS value_accessors;

\echo ''
\echo '=== 2. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_value_test() AS in_transaction;
COMMIT;

\echo ''
\echo '=== 3. the backend survived the transaction ==='
SELECT kwabi_value_test() AS after_transaction;

\echo ''
\echo '=== 4. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_value_control() AS control_should_not_return;

\echo ''
\echo '=== 5. the backend survived the control ==='
SELECT kwabi_value_test() AS after_control;

\echo ''
\echo '=== value-api tests complete ==='
