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
DROP FUNCTION IF EXISTS kwabi_hook_test_install_planner();
DROP FUNCTION IF EXISTS kwabi_hook_test_deny(boolean);
DROP FUNCTION IF EXISTS kwabi_hook_test_install_utility();
DROP FUNCTION IF EXISTS kwabi_hook_test_deny_utility(boolean);

CREATE FUNCTION kwabi_hook_test_install() RETURNS text
    AS :'bundle', 'kwabi_hook_test_install' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_clear() RETURNS boolean
    AS :'bundle', 'kwabi_hook_test_clear' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_trace() RETURNS text
    AS :'bundle', 'kwabi_hook_test_trace' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_refuse(boolean) RETURNS boolean
    AS :'bundle', 'kwabi_hook_test_refuse' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_install_planner() RETURNS text
    AS :'bundle', 'kwabi_hook_test_install_planner' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_deny(boolean) RETURNS boolean
    AS :'bundle', 'kwabi_hook_test_deny' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_install_utility() RETURNS text
    AS :'bundle', 'kwabi_hook_test_install_utility' LANGUAGE C;
CREATE FUNCTION kwabi_hook_test_deny_utility(boolean) RETURNS boolean
    AS :'bundle', 'kwabi_hook_test_deny_utility' LANGUAGE C;

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

-- 6. Permission check and planner. The planner body runs first for a statement, then
--    the start body and the permission-check body, then the run chain. Planner and
--    check bodies record only traced statements, as the other bodies do.
SELECT 'install planner: ' || kwabi_hook_test_install_planner() AS status_planner;
SELECT kwabi_hook_test_clear();
SELECT count(*) AS rows_with_planner FROM generate_series(1, 3) /* kwt_trace */;
DO $$
DECLARE tr text;
BEGIN
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check planner_and_check_order (trace=%): %', tr,
    CASE WHEN tr = 'PSC12FE' THEN 't' ELSE 'f' END;
END $$;

-- 7. A denial from the permission-check body reaches the statement with the body's
--    SQLSTATE and message, and the planner has already run.
SELECT kwabi_hook_test_deny(true);
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
  RAISE NOTICE 'check check_perms_denial (got %): %', r,
    CASE WHEN r = '42501|denied by kwabi test body' THEN 't' ELSE 'f' END;
  -- Inside a DO block PL/pgSQL plans an EXECUTE more than once, so the planner
  -- count is not fixed. What matters: the permission check is the last hook to run,
  -- and no run-side link (the body's '1' or '2', finish or end) ran after it.
  RAISE NOTICE 'check denial_stops_before_run (trace=%): %', tr,
    CASE WHEN tr ~ '^P+SC$' THEN 't' ELSE 'f' END;
END $$;
SELECT kwabi_hook_test_deny(false);

-- 8. Negative control for the new points: an untraced statement records nothing.
SELECT kwabi_hook_test_clear();
SELECT count(*) FROM generate_series(1, 3);
DO $$
DECLARE tr text;
BEGIN
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check untraced_with_planner_records_nothing (trace=%): %', tr,
    CASE WHEN tr = '' THEN 't' ELSE 'f' END;
END $$;

-- 9. Utility statements. A SET is a utility statement with no executor work, so the
--    only hook that runs is the utility body. Its trace is 'U' for a traced statement.
SELECT 'install utility: ' || kwabi_hook_test_install_utility() AS status_utility;
SELECT kwabi_hook_test_clear();
SET application_name = 'before' /* kwt_trace */;
DO $$
DECLARE tr text;
BEGIN
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check utility_records_u (trace=%): %', tr,
    CASE WHEN tr = 'U' THEN 't' ELSE 'f' END;
END $$;

-- 10. A standard error from a utility statement passes through the chain. The first
--     CREATE succeeds and the second raises 42P07.
DROP TABLE IF EXISTS kwt_trace_dup;
SELECT kwabi_hook_test_clear();
DO $$
DECLARE r text; tr text;
BEGIN
  BEGIN
    EXECUTE 'CREATE TEMP TABLE kwt_trace_dup (i int) /* kwt_trace */';
    EXECUTE 'CREATE TEMP TABLE kwt_trace_dup (i int) /* kwt_trace */';
    r := 'no error';
  EXCEPTION WHEN others THEN
    r := SQLSTATE;
  END;
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check utility_standard_error (got %): %', r,
    CASE WHEN r = '42P07' THEN 't' ELSE 'f' END;
  -- PL/pgSQL may plan and run the EXECUTE more than once, and the planner body also
  -- records here, so the check counts the utility entries rather than matching the
  -- whole trace. Both CREATEs must reach the utility chain.
  RAISE NOTICE 'check utility_trace_per_statement (trace=%): %', tr,
    CASE WHEN length(regexp_replace(tr, '[^U]', '', 'g')) >= 2 THEN 't' ELSE 'f' END;
END $$;

-- 11. A denied utility statement does not run: the setting keeps its old value.
SELECT kwabi_hook_test_deny_utility(true);
SELECT kwabi_hook_test_clear();
DO $$
DECLARE r text; tr text;
BEGIN
  BEGIN
    EXECUTE 'SET application_name = ''after'' /* kwt_trace */';
    r := 'no error';
  EXCEPTION WHEN others THEN
    r := SQLSTATE || '|' || SQLERRM;
  END;
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check utility_denial (got %): %', r,
    CASE WHEN r = '42501|utility denied by kwabi test body' THEN 't' ELSE 'f' END;
  RAISE NOTICE 'check denied_utility_did_not_run (setting=%): %', current_setting('application_name'),
    CASE WHEN current_setting('application_name') = 'before' THEN 't' ELSE 'f' END;
END $$;
SELECT kwabi_hook_test_deny_utility(false);

-- 12. Negative control: an untraced utility statement records nothing.
SELECT kwabi_hook_test_clear();
SET application_name = 'plain';
DO $$
DECLARE tr text;
BEGIN
  tr := kwabi_hook_test_trace();
  RAISE NOTICE 'check untraced_utility_records_nothing (trace=%): %', tr,
    CASE WHEN tr = '' THEN 't' ELSE 'f' END;
END $$;
