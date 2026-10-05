-- probe2.sql — the decisive measurement.
--
-- Run: make probe2 PG=18
--
-- fwprobe asked whether the backend survives a caught ERROR. It does. The
-- question that decides the design is whether the *work* done before the error
-- survives being swallowed. It must not: an extension that catches an error
-- and continues is asserting that the attempt had no effect.
--
-- Rows are tagged by source so a leftover from one case cannot be mistaken for
-- a leftover from another. Counting uses read_only=false, because read_only=true
-- skips CommandCounterIncrement and cannot see the caller's own writes.

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '=== decisive probes: does partial work survive a swallowed error? ==='

DROP TABLE IF EXISTS fw_t;
CREATE TABLE fw_t (tag int);

DROP FUNCTION IF EXISTS fw_partial_no_subxact(int4);
DROP FUNCTION IF EXISTS fw_partial_with_subxact(int4);

CREATE FUNCTION fw_partial_no_subxact(int4)
    RETURNS text AS :'module', 'fw_partial_no_subxact' LANGUAGE C;
CREATE FUNCTION fw_partial_with_subxact(int4)
    RETURNS text AS :'module', 'fw_partial_with_subxact' LANGUAGE C;

\echo ''
\echo '--- [1] insert tag=1, raise, swallow with PG_TRY alone ---'
SELECT fw_partial_no_subxact(1) AS result;
SELECT count(*) AS tag1_rows_now FROM fw_t WHERE tag = 1;

\echo ''
\echo '--- [2] insert tag=2 inside an internal subtransaction, then raise ---'
SELECT fw_partial_with_subxact(2) AS result;
SELECT count(*) AS tag2_rows_now FROM fw_t WHERE tag = 2;

\echo ''
\echo '--- [3] the summary: which tags survived? ---'
SELECT tag, count(*) AS rows FROM fw_t GROUP BY tag ORDER BY tag;

\echo ''
\echo '--- [4] one transaction, both paths, then commit ---'
TRUNCATE fw_t;
BEGIN;
SELECT fw_partial_no_subxact(10) AS a;
SELECT fw_partial_with_subxact(20) AS b;
COMMIT;
SELECT tag, count(*) AS rows_after_commit FROM fw_t GROUP BY tag ORDER BY tag;

\echo ''
\echo '--- [5] backend health ---'
SELECT count(*) AS client_backends FROM pg_stat_activity
 WHERE backend_type = 'client backend';

DROP TABLE IF EXISTS fw_t;

\echo ''
\echo '=== probes complete ==='
