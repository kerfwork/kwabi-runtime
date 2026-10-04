-- ========================================================================
-- fmgr-api.sql — the function-call group through the ABI.
--
-- This is the test for kata dak5 (fmgr-api). It proves an extension can look a
-- function up by OID and call it, with 1/2/3/N arguments, entirely through the
-- ABI — and that the two things a bare `Datum` return cannot express both work:
-- a NULL result, and a caught error.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message: a message grep is coupled to wording and to how
-- many times a line is printed, which is exactly how an earlier check passed
-- while printing nothing (see ci-local.sh's mem-api block).
--
-- ON_ERROR_STOP is off: check 8 raises by design (the negative control) and
-- check 9 expects a caught error, and the script must survive both.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_fmgr_control() asserts 5 == 6 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as mem-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_fmgr_call1(int4, int4);
DROP FUNCTION IF EXISTS kwabi_fmgr_call2(int4, int4, int4);
DROP FUNCTION IF EXISTS kwabi_fmgr_call3(int4, text, int4, int4);
DROP FUNCTION IF EXISTS kwabi_fmgr_calln(int4, int4, int4);
DROP FUNCTION IF EXISTS kwabi_fmgr_null_result();
DROP FUNCTION IF EXISTS kwabi_fmgr_error_code();
DROP FUNCTION IF EXISTS kwabi_fmgr_bad_lookup();
DROP FUNCTION IF EXISTS kwabi_fmgr_control();

CREATE FUNCTION kwabi_fmgr_call1(int4, int4)
    RETURNS int4 AS :'bundle','kwabi_fmgr_call1' LANGUAGE C;
CREATE FUNCTION kwabi_fmgr_call2(int4, int4, int4)
    RETURNS int4 AS :'bundle','kwabi_fmgr_call2' LANGUAGE C;
CREATE FUNCTION kwabi_fmgr_call3(int4, text, int4, int4)
    RETURNS text AS :'bundle','kwabi_fmgr_call3' LANGUAGE C;
CREATE FUNCTION kwabi_fmgr_calln(int4, int4, int4)
    RETURNS int4 AS :'bundle','kwabi_fmgr_calln' LANGUAGE C;
CREATE FUNCTION kwabi_fmgr_null_result()
    RETURNS bool AS :'bundle','kwabi_fmgr_null_result' LANGUAGE C;
CREATE FUNCTION kwabi_fmgr_error_code()
    RETURNS text AS :'bundle','kwabi_fmgr_error_code' LANGUAGE C;
CREATE FUNCTION kwabi_fmgr_bad_lookup()
    RETURNS bool AS :'bundle','kwabi_fmgr_bad_lookup' LANGUAGE C;
CREATE FUNCTION kwabi_fmgr_control()
    RETURNS bool AS :'bundle','kwabi_fmgr_control' LANGUAGE C;
DROP FUNCTION IF EXISTS kwabi_fmgr_ins_then_raise(int4, int4);
CREATE FUNCTION kwabi_fmgr_ins_then_raise(int4, int4)
    RETURNS text AS :'bundle','kwabi_fmgr_ins_then_raise' LANGUAGE C;

-- The target for check 13: writes a row, then raises. Called THROUGH the ABI,
-- so the slot is what must undo the write.
DROP FUNCTION IF EXISTS kwabi_ins_then_raise(int4);
CREATE FUNCTION kwabi_ins_then_raise(tag int4) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO kwabi_fmgr_t VALUES (tag);
    RAISE EXCEPTION 'kwabi: deliberate raise after a write';
END $$;

-- OIDs, identical on 16.15 / 17.11 / 18.6:
--   abs(int4)=1397  int4pl=177  int4div=154  substr(text,int4,int4)=877
--   current_setting(text,bool)=3294

\echo ''
\echo '=== 1. look a function up by OID, then call it with one argument ==='
\echo '   abs(int4) = 1397;  abs(-5) must be 5'
SELECT kwabi_fmgr_call1(1397, -5) = 5 AS one_arg_call;

\echo ''
\echo '=== 2. two-argument call ==='
\echo '   int4pl(2, 3) = 177'
SELECT kwabi_fmgr_call2(177, 2, 3) = 5 AS two_arg_call;

\echo ''
\echo '=== 3. the variadic call_function slot (shim builds the fcinfo) ==='
\echo '   PostgreSQL has no N-ary helper, so this path is hand-built and'
\echo '   must be tested on its own.'
SELECT kwabi_fmgr_calln(177, 40, 2) = 42 AS nary_call;

\echo ''
\echo '=== 4. three-argument call, mixed argument types ==='
\echo '   substr(text,int4,int4) = 877;  substr(''kwabi'', 1, 3) = ''kwa'''
SELECT kwabi_fmgr_call3(877, 'kwabi', 1, 3) = 'kwa' AS three_arg_call;

\echo ''
\echo '=== 5. a NULL RESULT is reported as NULL, not as 0 ==='
\echo '   current_setting(text,bool)=3294 returns NULL for a missing GUC'
\echo '   with missing_ok=true. This is the assertion the old bare-Datum'
\echo '   signature could not make: NULL and 0 would be indistinguishable.'
SELECT kwabi_fmgr_null_result() AS null_result_reported;

\echo ''
\echo '=== 6. a division result is correct (not just non-null) ==='
SELECT kwabi_fmgr_call2(154, 10, 2) = 5 AS division_correct;

\echo ''
\echo '=== 7. the error channel still reads clean after successful calls ==='
\echo '   (a stale error from an earlier statement must not leak into a'
\echo '    successful call; this is the same class as the ordering bug in'
\echo '    capability-consumer.sql)'
SELECT kwabi_fmgr_call2(177, 1, 1) = 2 AS call_after_success;

\echo ''
\echo '=== 8. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_fmgr_control() AS control_should_not_return;

\echo ''
\echo '=== 9. a function that RAISES is caught, with its SQLSTATE ==='
\echo '   int4div(1, 0) raises division_by_zero inside the slot. The slot'
\echo '   catches it and rolls back the subtransaction; the SQLSTATE must'
\echo '   come back through error_get as ''22012''. ''none'' means it was'
\echo '   swallowed (the call wrongly succeeded).'
SELECT kwabi_fmgr_error_code() = '22012' AS error_caught_with_sqlstate;

\echo ''
\echo '=== 10. a failed lookup returns NULL rather than crashing ==='
\echo '   fmgr_info(InvalidOid) makes PostgreSQL raise "cache lookup failed'
\echo '   for function 0"; the shim must catch it and return NULL.'
SELECT kwabi_fmgr_bad_lookup() AS bad_lookup_caught;

\echo ''
\echo '=== 13. THE load-bearing check: a failed call leaves NO partial work ==='
\echo '   kwabi_ins_then_raise inserts a row into kwabi_fmgr_t and then raises.'
\echo '   The slot catches the error AND must roll the insert back, so the'
\echo '   surviving row count must be 0. Without the subtransaction the row'
\echo '   would remain and could commit -- the silent-inconsistency bug.'
\echo ''
\echo '   Two things make this discriminate, both learned by measuring:'
\echo '   (a) it runs in an EXPLICIT transaction -- in autocommit the error'
\echo '       aborts the implicit transaction and discards the write anyway,'
\echo '       so the check would pass even with the rollback removed;'
\echo '   (b) the table is created fresh here, not reused -- a survivor left'
\echo '       by an earlier run (e.g. the stripped control bundle) would'
\echo '       otherwise be counted and flip the result.'
\echo ''
\echo '   Measured both ways: shipped bundle -> 0; a bundle with the'
\echo '   subtransaction removed -> 1. See subxact-probe.sql.'
DROP TABLE IF EXISTS kwabi_fmgr_t;
CREATE TABLE kwabi_fmgr_t (tag int);
BEGIN;
SELECT kwabi_fmgr_ins_then_raise(
         (SELECT oid::int4 FROM pg_proc
           WHERE proname = 'kwabi_ins_then_raise' AND pronamespace = 'public'::regnamespace),
         77001) = 'P0001' AS insert_then_raise_caught;
SELECT count(*) = 0 AS no_partial_work_survived
  FROM kwabi_fmgr_t WHERE tag = 77001;
ROLLBACK;
DROP TABLE IF EXISTS kwabi_fmgr_t;

\echo ''
\echo '=== 14. the backend survived all of the above ==='
SELECT kwabi_fmgr_call2(177, 20, 22) = 42 AS after_error_and_control;

\echo ''
\echo '=== 12. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_fmgr_call2(177, 100, 200) = 300 AS in_transaction;
COMMIT;

\echo ''
\echo '=== fmgr-api tests complete ==='
