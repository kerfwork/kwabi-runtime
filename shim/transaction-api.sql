-- ========================================================================
-- transaction-api.sql — the transaction group through the ABI.
--
-- This is the test for the transaction slots. It proves an extension can
-- start, commit, and abort transactions, check if a transaction is active,
-- and get the current transaction ID — entirely through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 5 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_transaction_control() asserts the xid is 999 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_transaction_test();
DROP FUNCTION IF EXISTS kwabi_transaction_abort_test();
DROP FUNCTION IF EXISTS kwabi_transaction_control();

CREATE FUNCTION kwabi_transaction_test()
    RETURNS text AS :'bundle','kwabi_transaction_test' LANGUAGE C;
CREATE FUNCTION kwabi_transaction_abort_test()
    RETURNS bool AS :'bundle','kwabi_transaction_abort_test' LANGUAGE C;
CREATE FUNCTION kwabi_transaction_control()
    RETURNS bool AS :'bundle','kwabi_transaction_control' LANGUAGE C;

\echo ''
\echo '=== 1. transaction_start + transaction_is_active + transaction_get_current_xid + transaction_commit ==='
\echo '   start must make is_active true, xid must be non-zero, commit must make is_active false'
SELECT kwabi_transaction_test() AS transaction_lifecycle;

\echo ''
\echo '=== 2. transaction_start + transaction_abort ==='
\echo '   start must make is_active true, abort must make is_active false'
SELECT kwabi_transaction_abort_test() AS transaction_abort;

\echo ''
\echo '=== 3. the backend survived all of the above ==='
SELECT kwabi_transaction_abort_test() AS after_all_checks;

\echo ''
\echo '=== 4. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_transaction_abort_test() AS in_transaction;
COMMIT;

\echo ''
\echo '=== 5. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_transaction_control() AS control_should_not_return;

\echo ''
\echo '=== transaction-api tests complete ==='
