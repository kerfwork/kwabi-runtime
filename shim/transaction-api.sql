-- Copyright (C) 2026 Simon Janes
-- SPDX-License-Identifier: AGPL-3.0-or-later

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
DROP FUNCTION IF EXISTS kwabi_transaction_status(int8);

CREATE FUNCTION kwabi_transaction_xid()
    RETURNS int8 AS :'bundle','kwabi_transaction_xid' LANGUAGE C;
CREATE FUNCTION kwabi_transaction_control()
    RETURNS bool AS :'bundle','kwabi_transaction_control' LANGUAGE C;
CREATE FUNCTION kwabi_transaction_status(int8)
    RETURNS int4 AS :'bundle','kwabi_transaction_status' LANGUAGE C;
DROP TABLE IF EXISTS kwabi_dead_t;
CREATE TABLE kwabi_dead_t (v int);

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
\echo '=== 2b. status of a running transaction is IN_PROGRESS (0) ==='
BEGIN;
INSERT INTO kwabi_txn_probe VALUES (2);
SELECT kwabi_transaction_status(kwabi_transaction_xid()) = 0 AS status_in_progress;
COMMIT;

\echo ''
\echo '=== 2c. status of a committed transaction is COMMITTED (1) ==='
BEGIN;
INSERT INTO kwabi_txn_probe VALUES (3);
SELECT kwabi_transaction_xid() AS committed_xid \gset
COMMIT;
SELECT kwabi_transaction_status(:committed_xid) = 1 AS status_committed;

\echo ''
\echo '=== 2d. status of a rolled-back transaction is ABORTED (2) ==='
BEGIN;
INSERT INTO kwabi_txn_probe VALUES (4);
SELECT kwabi_transaction_xid() AS rolled_back_xid \gset
ROLLBACK;
SELECT kwabi_transaction_status(:rolled_back_xid) = 2 AS status_rolled_back;

\echo ''
\echo '=== 2e. a backend killed mid-transaction reads as ABORTED (2) ==='
\echo '   a second session writes, the server reports its xid, the backend is terminated'
\set dead_xid `sh transaction-dead-backend.sh`
SELECT kwabi_transaction_status(:'dead_xid'::int8) = 2 AS status_dead_backend;

\echo ''
\echo '=== 2f. an xid not yet assigned is UNKNOWN (3); the status is a pure read ==='
SELECT kwabi_transaction_status(9223372036854775807) = 3 AS status_unknown_future;

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
