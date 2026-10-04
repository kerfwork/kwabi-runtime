-- ========================================================================
-- defrem-api.sql — the defrem (default) group through the ABI.
--
-- This is the test for the defrem kata. It proves an extension can create,
-- alter, and drop column defaults entirely through the ABI — no direct
-- PostgreSQL calls.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 5 raises by design (the negative control) and
-- the script must survive it.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_defrem_control() asserts a wrong value and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as mem-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_defrem_test();
DROP FUNCTION IF EXISTS kwabi_defrem_control();

CREATE FUNCTION kwabi_defrem_test()
    RETURNS bool AS :'bundle','kwabi_defrem_test' LANGUAGE C;
CREATE FUNCTION kwabi_defrem_control()
    RETURNS bool AS :'bundle','kwabi_defrem_control' LANGUAGE C;

\echo ''
\echo '=== 1. create, alter, and drop a column default ==='
\echo '   create a temp table, set a default through the ABI, verify it,'
\echo '   alter it, then drop it'
SELECT kwabi_defrem_test() AS defrem_lifecycle;

\echo ''
\echo '=== 2. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_defrem_test() AS in_transaction;
COMMIT;

\echo ''
\echo '=== 3. the backend survived the transaction ==='
SELECT kwabi_defrem_test() AS after_transaction;

\echo ''
\echo '=== 4. the backend is still alive ==='
SELECT kwabi_defrem_test() AS still_alive;

\echo ''
\echo '=== 5. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_defrem_control() AS control_should_not_return;

\echo ''
\echo '=== 6. the backend survived the control ==='
SELECT kwabi_defrem_test() AS after_control;

\echo ''
\echo '=== defrem-api tests complete ==='
