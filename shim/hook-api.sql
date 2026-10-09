-- ========================================================================
-- hook-api.sql — the executor hook chain through the ABI (kwabi.h "Executor hooks").
--
-- Run with: psql -v bundle='/abs/path/kwabi_runtime_pgNN.dylib' -f hook-api.sql
--
-- Each check prints "check <name>: t" or "check <name>: f". The harness counts them.
--
-- Hooks are per backend and cannot be removed, so this file runs in one session and
-- installs its bodies once. Statements that should be traced carry the marker
-- kwt_trace in their text; the bodies record only those.
--
-- NEGATIVE CONTROL. A chain that never runs would pass the trace checks as
-- vacuous. The untraced statement at the end must leave the trace empty, and the
-- error checks must see the error the standard function raised, not nothing.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_hook_test_install();
DROP FUNCTION IF EXISTS kwabi_hook_test_clear();
DROP FUNCTION IF EXISTS kwabi_hook_test_trace();
DROP FUNCTION IF EXISTS kwabi_hook_test_refuse(boolean);

CREATE FUNCTION kwabi_hook_test_install() RETURNS text
    AS :'bundle', 'kwabi_hook_test_install' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_clear() RETURNS boolean
    AS :'bundle', 'kwabi_hook_test_clear' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_trace() RETURNS text
    AS :'bundle', 'kwabi_hook_test_trace' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_refuse(boolean) RETURNS boolean
    AS :'bundle', 'kwabi_hook_test_refuse' LANGUAGE C;

SELECT 'install: ' || kwabi_hook_test_install() AS status;

-- 1. Chain order and argument passing. Start, then run bodies "1" then "2", the
--    standard run, then finish and end, one each.
SELECT kwabi_hook_test_clear();
SELECT count(*) AS rows_returned FROM generate_series(1, 3) /* kwt_trace */;
DO $$
BEGIN
  RAISE NOTICE 'check rows_pass_through: %',
    CASE WHEN (SELECT count(*) FROM generate_series(1, 3)) = 3 THEN 't' ELSE 'f' END;
END $$;
DO $$
DECLARE tr text;
BEGIN
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check chain_order (trace=%): %', tr,
    CASE WHEN tr = 'S12FE' THEN 't' ELSE 'f' END;
END $$;

-- 2. The standard link's error reaches the statement with its SQLSTATE and message.
--    Checked inside a DO block, so the error is caught and its fields compared.
SELECT kwabi_hook_test_clear();
DO $$
DECLARE r text; tr text;
BEGIN
  BEGIN
    EXECUTE 'SELECT 1/x FROM generate_series(0, 0) x /* kwt_trace */';
    r := 'no error';
  EXCEPTION WHEN others THEN
    r := SQLSTATE || '|' || SQLERRM;
  END;
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check standard_error_propagates (got %): %', r,
    CASE WHEN r = '22012|division by zero' THEN 't' ELSE 'f' END;
  RAISE NOTICE 'check error_skips_later_links (trace=%): %', tr,
    CASE WHEN tr = 'S12' THEN 't' ELSE 'f' END;
END $$;

-- 3. A body refuses. Its error must reach the statement with the body's SQLSTATE and
--    message, and the statement must not run. The next statement runs normally.
SELECT kwabi_hook_test_refuse(true);
SELECT kwabi_hook_test_clear();
DO $$
DECLARE r text; tr text;
BEGIN
  BEGIN
    EXECUTE 'SELECT count(*) FROM generate_series(1, 3) /* kwt_trace */';
    r := 'no error';
  EXCEPTION WHEN others THEN
    r := SQLSTATE || '|' || SQLERRM;
  END;
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check refusal_raises_body_error (got %): %', r,
    CASE WHEN r = 'P0001|kwabi hook refused the statement' THEN 't' ELSE 'f' END;
  RAISE NOTICE 'check refusal_stops_the_chain (trace=%): %', tr,
    CASE WHEN tr = 'S12' THEN 't' ELSE 'f' END;
END $$;
SELECT kwabi_hook_test_refuse(false);

-- 4. Recovery: with the refusal off, the same statement succeeds.
SELECT kwabi_hook_test_clear();
SELECT 'recovered: ' || count(*) AS recovery FROM generate_series(1, 3) /* kwt_trace */;

-- 5. Negative control: an untraced statement records nothing. If the trace were
--    recorded for every statement, the checks above would prove nothing about the marker.
SELECT kwabi_hook_test_clear();
SELECT count(*) FROM generate_series(1, 3);
DO $$
DECLARE tr text;
BEGIN
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check untraced_statement_records_nothing (trace=%): %', tr,
    CASE WHEN tr = '' THEN 't' ELSE 'f' END;
END $$;
