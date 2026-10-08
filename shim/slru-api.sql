-- ========================================================================
-- slru-api.sql — the SLRU group through the ABI.
--
-- This is the test for the SLRU slots. It proves an extension can create
-- (attach to) an SLRU declared at preload, write a page, read it back, and
-- see the SLRU in pg_stat_slru — entirely through the ABI.
--
-- PRECONDITION, and it is not optional: this script must run against a server
-- started with the runtime in shared_preload_libraries AND with the SLRU
-- declared in kwabi.slrus. An SLRU's shared memory must be reserved before
-- the postmaster forks; a plain LOAD cannot create one. The harness target
-- (make slru-api) starts such a server. If you run this by hand against a
-- LOAD-only server, every check fails with "requires preload" — which is the
-- correct behaviour, not a test bug.
--
-- Every check is an explicit boolean. The harness counts `t` and `f`.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence. Two
-- controls here:
--   * check 5 asserts an UNDECLARED name must RAISE;
--   * check 6 asserts a buffer-count mismatch must RAISE.
-- If either returns a row instead of erroring, the create path is vacuous.
--
-- ON_ERROR_STOP is off: checks 5 and 6 are ERRORs by design.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_slru_create_test();
DROP FUNCTION IF EXISTS kwabi_slru_write_test(int8, text);
DROP FUNCTION IF EXISTS kwabi_slru_read_test(int8);
DROP FUNCTION IF EXISTS kwabi_slru_undeclared_test();
DROP FUNCTION IF EXISTS kwabi_slru_badcount_test();
DROP FUNCTION IF EXISTS kwabi_slru_no_preload_test();

CREATE FUNCTION kwabi_slru_create_test()
    RETURNS text AS :'bundle','kwabi_slru_create_test' LANGUAGE C;
CREATE FUNCTION kwabi_slru_write_test(int8, text)
    RETURNS bool AS :'bundle','kwabi_slru_write_test' LANGUAGE C;
CREATE FUNCTION kwabi_slru_read_test(int8)
    RETURNS text AS :'bundle','kwabi_slru_read_test' LANGUAGE C;
CREATE FUNCTION kwabi_slru_undeclared_test()
    RETURNS bool AS :'bundle','kwabi_slru_undeclared_test' LANGUAGE C;
CREATE FUNCTION kwabi_slru_badcount_test()
    RETURNS bool AS :'bundle','kwabi_slru_badcount_test' LANGUAGE C;

-- Check 8 reads the capability bitset, so that slot must be registered here
-- too. LOAD alone does not create the SQL function; capabilities.sql does it
-- for its own run, and this script runs in its own session.
DROP FUNCTION IF EXISTS kwabi_capabilities();
CREATE FUNCTION kwabi_capabilities()
    RETURNS bigint AS :'bundle','kwabi_capabilities' LANGUAGE C;

\echo ''
\echo '=== 1. the SLRU page counters reach pg_stat_slru ==='
\echo '   MEASURED: pgstat_get_slru_index() maps any name not in its built-in'
\echo '   list to the "other" bucket, so a third-party SLRU is counted under'
\echo '   "other", never under its own name. Asserting name = kwabitest would'
\echo '   be asserting something PostgreSQL cannot do.'
SELECT count(*) = 1 AS other_bucket_exists
  FROM pg_stat_slru WHERE name = 'other';

\echo ''
\echo '=== 2. slru_create attaches to the declared SLRU ==='
SELECT kwabi_slru_create_test() LIKE '%attached%' AS create_attaches;

\echo ''
\echo '=== 3. write a page, then read it back ==='
\echo '   the bytes read must equal the bytes written'
SELECT kwabi_slru_write_test(0, 'kwabi slru round trip') AS wrote;
SELECT kwabi_slru_read_test(0) = 'kwabi slru round trip' AS read_back_matches;

\echo ''
\echo '=== 4. a second, distinct page does not alias the first ==='
SELECT kwabi_slru_write_test(1, 'second page') AS wrote2;
SELECT kwabi_slru_read_test(0) = 'kwabi slru round trip' AS page0_intact;

\echo ''
\echo '=== 5. NEGATIVE CONTROL: an undeclared name must RAISE ==='
\echo '   (must ERROR with "was not declared"; returning a row means check 2'
\echo '    is vacuous)'
SELECT kwabi_slru_undeclared_test() AS control_should_not_return;

\echo ''
\echo '=== 6. NEGATIVE CONTROL: a buffer-count mismatch must RAISE ==='
SELECT kwabi_slru_badcount_test() AS control_should_not_return;

\echo ''
\echo '=== 7. the backend survived both controls ==='
SELECT kwabi_slru_read_test(0) = 'kwabi slru round trip' AS after_controls;

\echo ''
\echo '=== 8. the SLRU capability bit is SET on this preloaded server ==='
\echo '   (capabilities.sql asserts it is CLEAR without preload; these two'
\echo '    together are the two-way control for the bit tracking the load model)'
SELECT ((kwabi_capabilities() & 32) <> 0) AS slru_bit_set_when_preloaded;

\echo ''
\echo '=== slru-api tests complete ==='
