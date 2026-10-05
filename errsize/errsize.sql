-- errsize.sql — cross-version safety for the structured error channel.
--
-- Run: make test PG=18
--
-- The claim: a runtime built against a newer header, handed an error struct
-- from a caller built against an older one, writes only what fits.
--
-- ON_ERROR_STOP is ON: every check here must pass, unlike the firewall tests
-- where some failures are the measurement.

\set ON_ERROR_STOP on
\pset pager off

\echo ''
\echo '=== structured error channel: cross-version safety ==='
SELECT version() AS server;

DROP FUNCTION IF EXISTS err_size_v1();
DROP FUNCTION IF EXISTS err_size_v2();
DROP FUNCTION IF EXISTS err_write_v1_caller();
DROP FUNCTION IF EXISTS err_write_v2_caller();

CREATE FUNCTION err_size_v1()
    RETURNS int4 AS :'module', 'err_size_v1' LANGUAGE C;
CREATE FUNCTION err_size_v2()
    RETURNS int4 AS :'module', 'err_size_v2' LANGUAGE C;
CREATE FUNCTION err_write_v1_caller()
    RETURNS text AS :'module', 'err_write_v1_caller' LANGUAGE C;
CREATE FUNCTION err_write_v2_caller()
    RETURNS text AS :'module', 'err_write_v2_caller' LANGUAGE C;

\echo ''
\echo '--- [1] the two struct sizes differ (so the risk is real) ---'
SELECT err_size_v1() AS v1_bytes, err_size_v2() AS v2_bytes,
       err_size_v2() > err_size_v1() AS v2_is_larger;

\echo ''
\echo '--- [2] a v1 caller gets core fields, and NO overrun ---'
\echo '(overrun_bytes must be 0)'
SELECT err_write_v1_caller() AS v1_caller;

\echo ''
\echo '--- [3] a v2 caller gets the optional fields too ---'
SELECT err_write_v2_caller() AS v2_caller;

\echo ''
\echo '--- [4] assert the guard band held, as a pass/fail ---'
WITH r AS (SELECT err_write_v1_caller() AS s)
SELECT
  (s LIKE '%overrun_bytes=0%')                       AS no_overrun,
  (s LIKE '%status=1%')                              AS status_written,
  (s LIKE '%sqlerrcode=327680%')                     AS sqlstate_written,
  (s LIKE '%extension reported a failure%')          AS message_written
FROM r;

\echo ''
\echo '=== done ==='
