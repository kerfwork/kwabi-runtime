-- ========================================================================
-- transaction-api.sql — the transaction group through the ABI.
--
-- There is ONE transaction slot: transaction_get_current_xid. An extension
-- reached from SQL is already inside a transaction and cannot start, commit or
-- abort one, so there is no lifecycle to test. See the Transactions block in
-- kwabi.h for the reasoning. What this test proves is the property the one
-- accessor must have: it reports the current transaction id when one is
-- assigned, and it is a pure read -- asking does NOT assign one.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: the negative control raises by design.
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

DROP FUNCTION IF EXISTS kwabi_transaction_xid();
DROP FUNCTION IF EXISTS kwabi_transaction_control();

CREATE FUNCTION kwabi_transaction_xid()
    RETURNS int8 AS :'bundle','kwabi_transaction_xid' LANGUAGE C;
CREATE FUNCTION kwabi_transaction_control()
    RETURNS bool AS :'bundle','kwabi_transaction_control' LANGUAGE C;

\echo ''
\echo '=== 1. a write inside a transaction assigns an XID, and the accessor sees it ==='
\echo '   after a write the xid must be non-zero'
BEGIN;
CREATE TEMP TABLE kwabi_txn_probe(x int);
INSERT INTO kwabi_txn_probe VALUES (1);
SELECT kwabi_transaction_xid() <> 0 AS transaction_lifecycle;
COMMIT;

\echo ''
\echo '=== 2. the accessor is a PURE READ: in a read-only transaction no XID exists ==='
\echo '   GetTopTransactionIdIfAny, not GetCurrentTransactionId -- asking must not'
\echo '   allocate an XID. A read-only transaction reports 0.'
SELECT kwabi_transaction_xid() = 0 AS unassigned_xid_is_zero;

\echo ''
\echo '=== 3. the backend survived all of the above ==='
SELECT true AS after_all_checks;

\echo ''
\echo '=== 4. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_transaction_control() AS control_should_not_return;

\echo ''
\echo '=== transaction-api tests complete ==='
