-- probe.sql — measure what PostgreSQL actually leaves behind after a caught ERROR.
--
-- Run: make probe PG=18
--
-- The question the whole firewall design turns on: when an ERROR is caught
-- with PG_TRY, is the transaction still usable, or does the caller have to
-- wrap the attempt in a subtransaction?
--
-- ON_ERROR_STOP is off: several of these are expected to fail, and the failure
-- is the measurement.

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '=== error-firewall probes ==='
SELECT version() AS server;

DROP FUNCTION IF EXISTS fw_no_subxact();
DROP FUNCTION IF EXISTS fw_with_subxact();
DROP FUNCTION IF EXISTS fw_capture_only();

CREATE FUNCTION fw_no_subxact()
    RETURNS text AS :'module', 'fw_no_subxact' LANGUAGE C;
CREATE FUNCTION fw_with_subxact()
    RETURNS text AS :'module', 'fw_with_subxact' LANGUAGE C;
CREATE FUNCTION fw_capture_only()
    RETURNS text AS :'module', 'fw_capture_only' LANGUAGE C;

\echo ''
\echo '--- [1] catch + flush only, no subtransaction, then allocate ---'
\echo '(if this errors, the catch alone leaves the backend unable to work)'
SELECT fw_no_subxact() AS no_subxact;

\echo ''
\echo '--- [2] is the transaction still usable after that? ---'
SELECT 1 AS still_usable;
SELECT txid_current_if_assigned() AS xid;

\echo ''
\echo '--- [3] catch + subtransaction + restore (the plpgsql pattern) ---'
SELECT fw_with_subxact() AS with_subxact;

\echo ''
\echo '--- [4] usable afterwards, inside an explicit transaction? ---'
BEGIN;
SELECT fw_with_subxact() AS in_txn_1;
SELECT fw_with_subxact() AS in_txn_2;
SELECT 42 AS still_alive;
COMMIT;

\echo ''
\echo '--- [5] nested: catch an error while already inside a subtransaction ---'
BEGIN;
SAVEPOINT sp1;
SELECT fw_with_subxact() AS nested_catch;
RELEASE SAVEPOINT sp1;
SELECT 'nested ok' AS nested_result;
COMMIT;

\echo ''
\echo '--- [6] capture only: does the message survive without a context switch? ---'
SELECT fw_capture_only() AS capture_only;

\echo ''
\echo '--- [7] repeated catches: does state leak across calls? ---'
SELECT count(*) AS ten_catches FROM (
  SELECT fw_with_subxact() FROM generate_series(1, 10)
) s;

\echo ''
\echo '--- [8] backend health ---'
SELECT count(*) AS client_backends FROM pg_stat_activity
 WHERE backend_type = 'client backend';

\echo ''
\echo '=== probes complete ==='

\echo ''
\echo '--- [9] WRONG ordering: copy the error AFTER rolling back ---'
\echo '(the error data lives in the subxact context; expect corruption or error)'
DROP FUNCTION IF EXISTS fw_copy_after_rollback();
CREATE FUNCTION fw_copy_after_rollback()
    RETURNS text AS :'module', 'fw_copy_after_rollback' LANGUAGE C;
SELECT fw_copy_after_rollback() AS wrong_order;

\echo ''
\echo '--- [10] ordering: copy AFTER rollback (context already in the subxact) ---'
DROP FUNCTION IF EXISTS fw_copy_after_rollback();
CREATE FUNCTION fw_copy_after_rollback()
    RETURNS text AS :'module', 'fw_copy_after_rollback' LANGUAGE C;
SELECT fw_copy_after_rollback() AS wrong_order;

\echo ''
\echo '--- [11] ordering: copy BEFORE rollback (the rule) ---'
DROP FUNCTION IF EXISTS fw_copy_before_rollback();
CREATE FUNCTION fw_copy_before_rollback()
    RETURNS text AS :'module', 'fw_copy_before_rollback' LANGUAGE C;
SELECT fw_copy_before_rollback() AS correct_order;

\echo ''
\echo '--- [12] backend health after the ordering probes ---'
SELECT count(*) AS client_backends FROM pg_stat_activity
 WHERE backend_type = 'client backend';
