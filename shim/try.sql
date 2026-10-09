-- try.sql — proof for kwabi_try (the error firewall's catching direction).
--
-- Run: make try PG=18
--
-- The contract being tested is in kwabi.h: a guarded body must not raise; it
-- signals failure by returning non-OK. Everything here checks that the
-- contract is enforceable and that violating it is survivable.

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '=== kwabi_try proof ==='
SELECT version() AS server;

DROP TABLE IF EXISTS kwabi_t;
CREATE TABLE kwabi_t (tag int);

\echo ''
\echo '--- [0] load the runtime and expose the proof functions ---'
LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_try_ok();
DROP FUNCTION IF EXISTS kwabi_try_body_raises();
DROP FUNCTION IF EXISTS kwabi_try_clean_failure();
DROP FUNCTION IF EXISTS kwabi_try_write_then_fail(int4);
DROP FUNCTION IF EXISTS kwabi_try_nested(int4);
DROP FUNCTION IF EXISTS kwabi_try_panics(text);

CREATE FUNCTION kwabi_try_ok()
    RETURNS text AS :'bundle', 'kwabi_try_ok' LANGUAGE C;
CREATE FUNCTION kwabi_try_body_raises()
    RETURNS text AS :'bundle', 'kwabi_try_body_raises' LANGUAGE C;
CREATE FUNCTION kwabi_try_clean_failure()
    RETURNS text AS :'bundle', 'kwabi_try_clean_failure' LANGUAGE C;
CREATE FUNCTION kwabi_try_write_then_fail(int4)
    RETURNS text AS :'bundle', 'kwabi_try_write_then_fail' LANGUAGE C;
CREATE FUNCTION kwabi_try_nested(int4)
    RETURNS text AS :'bundle', 'kwabi_try_nested' LANGUAGE C;
CREATE FUNCTION kwabi_try_panics(text)
    RETURNS text AS :'bundle', 'kwabi_try_panics' LANGUAGE C;

\echo ''
\echo '--- [1] happy path: body succeeds, work commits ---'
SELECT kwabi_try_ok() AS happy_path;

\echo ''
\echo '--- [2] body reports failure without raising ---'
SELECT kwabi_try_clean_failure() AS clean_failure;

\echo ''
\echo '--- [3] THE test: body writes a row, then fails ---'
\echo '(the row must NOT survive; the subtransaction must undo it)'
SELECT kwabi_try_write_then_fail(1) AS write_then_fail;
SELECT count(*) AS tag1_rows FROM kwabi_t WHERE tag = 1;

\echo ''
\echo '--- [4] contract violation: body raises a real ERROR ---'
\echo '(survivable, but the body broke the rule; expect status=1 sqlstate=22012)'
SELECT kwabi_try_body_raises() AS body_raises;

\echo ''
\echo '--- [5] nested try: inner failure must not leak to the outer ---'
SELECT kwabi_try_nested(2) AS nested;
SELECT count(*) AS tag2_rows FROM kwabi_t WHERE tag = 2;

\echo ''
\echo '--- [6] a body that PANICS (Rust) ---'
\echo '(KNOWN LIMITATION: this ABORTS the backend, so it runs LAST and in its own'
\echo ' session. The panic is a foreign exception to the runtime Rust std and'
\echo ' catch_unwind refuses it. See error-firewall-design.md section 4.)'
\echo '(skipped here; see panic-probe.sql, run separately)'
-- SELECT kwabi_try_panics(:'libdir' || '/' || :'canary') AS panics;

\echo ''
\echo '--- [7] everything still works after the panic ---'
SELECT kwabi_try_ok() AS after_panic;
SELECT kwabi_try_write_then_fail(3) AS after_panic_write;

\echo ''
\echo '--- [8] repeated tries: no leak of rows or state ---'
SELECT count(*) AS twenty_tries FROM (
  SELECT kwabi_try_write_then_fail(100 + g) FROM generate_series(1, 20) g
) s;
SELECT count(*) AS leaked_rows FROM kwabi_t WHERE tag >= 100;

\echo ''
\echo '--- [9] inside an explicit transaction, then commit ---'
BEGIN;
SELECT kwabi_try_write_then_fail(50) AS in_txn;
COMMIT;
SELECT count(*) AS tag50_rows_after_commit FROM kwabi_t WHERE tag = 50;

\echo ''
\echo '--- [10] backend health ---'
SELECT count(*) AS client_backends FROM pg_stat_activity
 WHERE backend_type = 'client backend';

DROP TABLE IF EXISTS kwabi_t;

\echo ''
DROP FUNCTION IF EXISTS kwabi_error_slots_test();
CREATE FUNCTION kwabi_error_slots_test()
    RETURNS bool AS :'bundle','kwabi_error_slots_test' LANGUAGE C;

\echo '=== error_slots: error_message/error_code read the captured error; error_clear empties it ==='
SELECT kwabi_error_slots_test() AS error_slots;

\echo '=== proof complete ==='
