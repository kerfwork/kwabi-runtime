-- ========================================================================
-- aggregate-api.sql — bound aggregate bodies through the runtime (design: notes/aggregate-design.md).
--
-- Run with: psql -v bundle='<runtime bundle>' -v v1='<body, scale 1>' -v v2='<body, scale 1000>' -f aggregate-api.sql
-- The runtime must be preloaded. The body is aggregate-api/agg_body.c.
--
-- Each check prints "check <name>: t" or "check <name>: f". The harness counts them.
--
-- NEGATIVE CONTROLS. A check that has only ever passed is not evidence. Where a check
-- compares two things, a control compares them against something that must differ.
-- ========================================================================

\set ON_ERROR_STOP off

CREATE FUNCTION kwabi_hook_test_bind(text, text) RETURNS text
    AS :'bundle', 'kwabi_hook_test_bind' LANGUAGE C STRICT;
CREATE FUNCTION kwabi_capabilities() RETURNS bigint
    AS :'bundle', 'kwabi_capabilities' LANGUAGE C STRICT;

-- The binding "ksum": one set of C symbols, one function per role.
CREATE FUNCTION ksum__step(internal, bigint) RETURNS internal AS :'bundle', 'kwabi_agg_step' LANGUAGE C;
CREATE FUNCTION ksum__final(internal) RETURNS bigint AS :'bundle', 'kwabi_agg_final' LANGUAGE C;
CREATE FUNCTION ksum__inverse(internal, bigint) RETURNS internal AS :'bundle', 'kwabi_agg_inverse' LANGUAGE C;
CREATE FUNCTION ksum__combine(internal, internal) RETURNS internal AS :'bundle', 'kwabi_agg_combine' LANGUAGE C;
CREATE FUNCTION ksum__serialize(internal) RETURNS bytea AS :'bundle', 'kwabi_agg_serialize' LANGUAGE C STRICT;
CREATE FUNCTION ksum__deserialize(bytea, internal) RETURNS internal AS :'bundle', 'kwabi_agg_deserialize' LANGUAGE C;

CREATE AGGREGATE ksum(bigint) (SFUNC = ksum__step, STYPE = internal, FINALFUNC = ksum__final,
                               SERIALFUNC = ksum__serialize, DESERIALFUNC = ksum__deserialize,
                               COMBINEFUNC = ksum__combine, PARALLEL = SAFE);
CREATE AGGREGATE ksum_mov(bigint) (SFUNC = ksum__step, STYPE = internal, FINALFUNC = ksum__final,
                                   MSFUNC = ksum__step, MSTYPE = internal,
                                   MINVFUNC = ksum__inverse, MFINALFUNC = ksum__final);

CREATE TABLE t_small (i int PRIMARY KEY, v bigint);
INSERT INTO t_small SELECT g, 2 * g FROM generate_series(1, 100) g;      -- values 2..200, no 13
CREATE TABLE t_big (i int PRIMARY KEY, v bigint);
INSERT INTO t_big SELECT g, 2 * g FROM generate_series(1, 200000) g;
CREATE TABLE t_err (i int PRIMARY KEY, v bigint);
INSERT INTO t_err SELECT g, g FROM generate_series(1, 20) g;             -- contains 13

SELECT kwabi_hook_test_bind('ksum', :'v1') AS bound_v1;
-- psql does not substitute :variables inside $$ bodies, so the v2 path goes through a setting.
SET kwabi_hook_test.v2_path = :'v2';

DO $$
BEGIN
  RAISE NOTICE 'check capability bit set when preloaded: %', CASE WHEN (kwabi_capabilities() & 256) <> 0 THEN 't' ELSE 'f' END;
END $$;

-- 1. A plain sum: 2 * (1 + ... + 100) = 10100.
DO $$
BEGIN
  RAISE NOTICE 'check sum of values: %', CASE WHEN (SELECT ksum(v) FROM t_small) = 10100 THEN 't' ELSE 'f' END;
END $$;

-- 2. Nulls are skipped, and an all-null or empty input is NULL.
INSERT INTO t_small VALUES (101, NULL);
DO $$
BEGIN
  RAISE NOTICE 'check null row is skipped: %', CASE WHEN (SELECT ksum(v) FROM t_small) = 10100 THEN 't' ELSE 'f' END;
END $$;
DELETE FROM t_small WHERE i = 101;
DO $$
BEGIN
  RAISE NOTICE 'check empty input is NULL: %', CASE WHEN (SELECT ksum(v) FROM t_small WHERE i > 1000) IS NULL THEN 't' ELSE 'f' END;
END $$;

-- 3. Parallel and serial agree: this exercises serialise, deserialise and combine, because
--    the aggregate is PARALLEL SAFE and the plan splits it between workers and the leader.
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
DO $$
DECLARE parallel_sum bigint; plan_line text; saw_partial boolean := false;
BEGIN
  parallel_sum := (SELECT ksum(v) FROM t_big);
  FOR plan_line IN EXECUTE 'EXPLAIN SELECT ksum(v) FROM t_big' LOOP
    IF plan_line LIKE '%Partial Aggregate%' THEN saw_partial := true; END IF;
  END LOOP;
  RAISE NOTICE 'check plan partially aggregates in workers: %', CASE WHEN saw_partial THEN 't' ELSE 'f' END;
  RAISE NOTICE 'check parallel sum matches the serial sum (40000200000): %',
    CASE WHEN parallel_sum = 40000200000 THEN 't' ELSE 'f' END;
END $$;
SET max_parallel_workers_per_gather = 0;
DO $$
BEGIN
  RAISE NOTICE 'check serial sum matches the same constant: %',
    CASE WHEN (SELECT ksum(v) FROM t_big) = 40000200000 THEN 't' ELSE 'f' END;
END $$;
SET max_parallel_workers_per_gather = 2;

-- 4. The moving aggregate, with the inverse, equals the built-in windowed sum at every row.
--    Control: the same comparison against a different frame must disagree somewhere.
DO $$
DECLARE mismatches int; control_mismatches int;
BEGIN
  SELECT count(*) INTO mismatches FROM (
    SELECT ksum_mov(v) OVER w AS ours, sum(v) OVER w AS builtin
    FROM t_small
    WINDOW w AS (ORDER BY i ROWS BETWEEN 2 PRECEDING AND CURRENT ROW)) x
  WHERE ours IS DISTINCT FROM builtin;
  SELECT count(*) INTO control_mismatches FROM (
    SELECT ksum_mov(v) OVER w AS ours, sum(v) OVER w2 AS builtin
    FROM t_small
    WINDOW w AS (ORDER BY i ROWS BETWEEN 2 PRECEDING AND CURRENT ROW),
           w2 AS (ORDER BY i ROWS BETWEEN 3 PRECEDING AND CURRENT ROW)) x
  WHERE ours IS DISTINCT FROM builtin;
  RAISE NOTICE 'check moving sum with inverse equals built-in window (0 mismatches): %',
    CASE WHEN mismatches = 0 THEN 't' ELSE 'f' END;
  RAISE NOTICE 'check control: a different frame does disagree (>0): %',
    CASE WHEN control_mismatches > 0 THEN 't' ELSE 'f' END;
END $$;

-- 5. A body error in step reaches the statement with its SQLSTATE and message. The control
--    range does not reach the refused value and succeeds.
DO $$
DECLARE r text;
BEGIN
  BEGIN
    PERFORM ksum(v) FROM t_err;
    r := 'no error';
  EXCEPTION WHEN others THEN
    r := SQLSTATE || '|' || SQLERRM;
  END;
  RAISE NOTICE 'check step error propagates (got %): %', r,
    CASE WHEN r = '22003|unlucky value 13' THEN 't' ELSE 'f' END;
  RAISE NOTICE 'check control: a range without 13 succeeds: %',
    CASE WHEN (SELECT ksum(v) FROM t_err WHERE i <= 12) = 78 THEN 't' ELSE 'f' END;
END $$;

-- 6. Pinning. The aggregate binds version 2 while it is running, at row 50. Its state was
--    created under version 1 at row 1, so it finishes under version 1: 10100, not 10100000.
DO $$
DECLARE r bigint;
BEGIN
  r := (SELECT ksum(CASE WHEN i = 50
                         THEN v + (kwabi_hook_test_bind('ksum', current_setting('kwabi_hook_test.v2_path')) = 'bound')::int * 0
                         ELSE v END)
        FROM t_small);
  RAISE NOTICE 'check running aggregate keeps its pinned version (10100): %',
    CASE WHEN r = 10100 THEN 't' ELSE 'f' END;
END $$;

-- 7. A new aggregate after the bind uses version 2: 10100 * 1000.
DO $$
BEGIN
  RAISE NOTICE 'check a new aggregate uses the new version (10100000): %',
    CASE WHEN (SELECT ksum(v) FROM t_small) = 10100000 THEN 't' ELSE 'f' END;
END $$;

-- 8. Under version 2, the parallel path agrees with the serial path, so the workers also
--    pick up the new binding.
DO $$
DECLARE parallel_sum bigint;
BEGIN
  SET LOCAL max_parallel_workers_per_gather = 2;
  parallel_sum := (SELECT ksum(v) FROM t_big);
  RAISE NOTICE 'check parallel under the new version (40000200000000): %',
    CASE WHEN parallel_sum = 40000200000000 THEN 't' ELSE 'f' END;
END $$;

-- Not covered here, and why: combine refusal across two body images (0A000). The two
-- states would have to come from different images in one aggregation, and a reload only
-- changes what new states use. It is covered at the C level by the same_body() check in
-- group_aggregate.c; a SQL test needs a plan that straddles a reload, which we do not have.
