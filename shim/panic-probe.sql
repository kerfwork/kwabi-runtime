-- panic-probe.sql — the panic limitation, isolated.
--
-- Run SEPARATELY from try.sql: it aborts the backend, which takes down every
-- other connection to that server. That is the finding, not a bug in the test.
--
--   psql -p 5432 -f panic-probe.sql postgres
--
-- Expected: the SELECT does not return; psql reports the connection was lost;
-- the server log shows the panic, then "Rust cannot catch foreign exceptions,
-- aborting", then postmaster terminating all other server processes and
-- running crash recovery.

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '=== panic probe: a guarded body that panics ==='
\echo 'The body lives in libcanary, a separately built artifact with its own'
\echo 'copy of Rust std. Its panic is therefore a FOREIGN EXCEPTION to the'
\echo 'runtime Rust std, and catch_unwind refuses to catch it.'

LOAD 'kwabi_runtime_pg18';

DROP FUNCTION IF EXISTS kwabi_try_panics(text);
CREATE FUNCTION kwabi_try_panics(text)
    RETURNS text AS 'kwabi_runtime_pg18', 'kwabi_try_panics' LANGUAGE C;

\echo ''
\echo '--- the call that aborts the backend ---'
SELECT kwabi_try_panics('/opt/homebrew/lib/postgresql@18/libcanary.dylib') AS panics;

\echo ''
\echo '--- not reached: the connection is gone by now ---'
SELECT 'still alive' AS after;
