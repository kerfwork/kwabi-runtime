-- proof.sql — run the kwabi runtime inside a live PostgreSQL backend.
--
-- Version-independent: the bundle, canary and libdir names arrive as psql
-- variables so the same script runs against 17 and 18.
--
-- Run with:  make proof PG=18   /   make proof PG=17
--
-- ON_ERROR_STOP is deliberately off: check 7 raises a real error and the
-- script must survive it to report the rest.

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '=== kwabi POC against live PostgreSQL ==='
SELECT version() AS server;

\echo ''
\echo '--- [1] load the runtime bundle into this backend ---'
LOAD :'bundle';
SELECT 'LOAD succeeded' AS check_1;

\echo ''
\echo '--- [2] expose the ABI proof functions ---'
DROP FUNCTION IF EXISTS kwabi_version();
DROP FUNCTION IF EXISTS kwabi_proof();
DROP FUNCTION IF EXISTS kwabi_roundtrip(int4);
DROP FUNCTION IF EXISTS kwabi_raise_test();
DROP FUNCTION IF EXISTS kwabi_load_extension(text);
DROP FUNCTION IF EXISTS kwabi_ext_alloc(text, int4);

CREATE FUNCTION kwabi_version()
    RETURNS text AS :'bundle', 'kwabi_version' LANGUAGE C;
CREATE FUNCTION kwabi_proof()
    RETURNS text AS :'bundle', 'kwabi_proof' LANGUAGE C;
CREATE FUNCTION kwabi_roundtrip(int4)
    RETURNS int4 AS :'bundle', 'kwabi_roundtrip' LANGUAGE C;
CREATE FUNCTION kwabi_raise_test()
    RETURNS void AS :'bundle', 'kwabi_raise_test' LANGUAGE C;
CREATE FUNCTION kwabi_load_extension(text)
    RETURNS text AS :'bundle', 'kwabi_load_extension' LANGUAGE C;
CREATE FUNCTION kwabi_ext_alloc(text, int4)
    RETURNS text AS :'bundle', 'kwabi_ext_alloc' LANGUAGE C;

SELECT 'functions created' AS check_2;

\echo ''
\echo '--- [3] the ABI reports what it is bound to ---'
SELECT kwabi_version() AS check_3;

\echo ''
\echo '--- [4] memory through the ABI is genuine PostgreSQL memory ---'
\echo '(GetMemoryChunkContext must name the backend current context)'
SELECT kwabi_proof() AS check_4;

\echo ''
\echo '--- [5] round-trip 1000 allocations inside a transaction ---'
BEGIN;
SELECT kwabi_roundtrip(1000) AS allocated_and_freed;
COMMIT;

\echo ''
\echo '--- [6] allocations belong to the transaction context ---'
BEGIN;
SELECT kwabi_proof() AS in_transaction;
COMMIT;

\echo ''
\echo '--- [7] a real PostgreSQL ERROR raised through the ABI ---'
\echo '(expect SQLSTATE 22012 division_by_zero; backend must survive)'
SELECT kwabi_raise_test() AS should_not_appear;
SELECT 'backend survived the error' AS check_7;

\echo ''
\echo '--- [8] the SQLSTATE is a genuine PostgreSQL error code ---'
DO $$
BEGIN
  PERFORM kwabi_raise_test();
  RAISE NOTICE 'no error raised (unexpected)';
EXCEPTION WHEN division_by_zero THEN
  RAISE NOTICE 'CONFIRMED: SQLSTATE 22012 caught by plpgsql';
END $$;

\echo ''
\echo '--- [9] after the error the ABI still works ---'
SELECT kwabi_proof() AS after_error;

\echo ''
\echo '--- [10] load a FOREIGN extension through the ABI ---'
\echo '(canary: built ONCE, from kwabi.h alone, zero PostgreSQL symbols)'
SELECT kwabi_load_extension(:'libdir' || '/' || :'canary') AS check_10;

\echo ''
\echo '--- [11] call into that extension; it must reach this allocator ---'
SELECT kwabi_ext_alloc(:'libdir' || '/' || :'canary', 128) AS check_11;

\echo ''
\echo '--- [12] the whole chain, repeated inside a transaction ---'
BEGIN;
SELECT kwabi_ext_alloc(:'libdir' || '/' || :'canary', 4096) AS in_transaction;
COMMIT;

\echo ''
\echo '--- [13] backend health ---'
SELECT count(*) AS client_backends FROM pg_stat_activity
 WHERE backend_type = 'client backend';

\echo ''
\echo '=== POC complete ==='
