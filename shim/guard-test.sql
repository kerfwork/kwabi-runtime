-- guard-test.sql — "must not panic" made true by construction.
--
-- Run: make guard-test PG=18
--
-- The test is a pair. The guarded body must be contained; the unguarded one
-- must abort. Without the control, "it did not crash" proves nothing — the
-- body might simply not have panicked.
--
-- The abort in the control is expected and takes down the connection, so the
-- control runs last and in its own psql invocation. See guard-test.sh.

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '=== #[guarded_body]: panic containment ==='

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_try_symbol(text, text, int4);
CREATE FUNCTION kwabi_try_symbol(text, text, int4)
    RETURNS text AS :'bundle', 'kwabi_try_symbol' LANGUAGE C;

\echo ''
\echo '--- [1] guarded body, no panic: must succeed ---'
SELECT kwabi_try_symbol(:'libdir' || '/' || :'guarded', 
                        'guarded_succeeds__kwabi_body', 0) AS succeeds;

\echo ''
\echo '--- [2] guarded body that reports a failure via Result ---'
SELECT kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                        'guarded_reports_failure__kwabi_body', 1) AS reports_failure;

\echo ''
\echo '--- [3] THE test: guarded body that PANICS ---'
\echo '(status must be 2 = KWABI_ERR_PANICKED; backend must survive)'
SELECT kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                        'guarded_maybe_panics__kwabi_body', 1) AS guarded_panic;

\echo ''
\echo '--- [4] backend survived, and still works ---'
SELECT 'alive' AS after_guarded_panic;
SELECT kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                        'guarded_succeeds__kwabi_body', 0) AS still_working;

\echo ''
\echo '--- [5] panic inside a transaction: the transaction is still usable ---'
BEGIN;
SELECT kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                        'guarded_maybe_panics__kwabi_body', 1) AS in_txn;
SELECT 42 AS txn_usable;
COMMIT;

\echo ''
\echo '--- [6] backend health ---'
SELECT count(*) AS client_backends FROM pg_stat_activity
 WHERE backend_type = 'client backend';

\echo ''
\echo '=== guarded-body tests complete ==='
\echo '(the unguarded control runs separately: make guard-control)'
