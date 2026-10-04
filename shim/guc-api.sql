-- ========================================================================
-- guc-api.sql — the GUC group through the ABI.
--
-- This is the test for the GUC kata. It proves an extension can read and
-- write GUC values entirely through the ABI — no direct PostgreSQL calls.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message: a message grep is coupled to wording and to how
-- many times a line is printed, which is exactly how an earlier check passed
-- while printing nothing.
--
-- ON_ERROR_STOP is off: check 8 raises by design (the negative control) and
-- the script must survive it.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_guc_control() asserts a wrong value and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as mem-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_guc_test_int(int4);
DROP FUNCTION IF EXISTS kwabi_guc_test_string(text);
DROP FUNCTION IF EXISTS kwabi_guc_test_bool(bool);
DROP FUNCTION IF EXISTS kwabi_guc_test_float(float8);
DROP FUNCTION IF EXISTS kwabi_guc_set_test();
DROP FUNCTION IF EXISTS kwabi_guc_control();

CREATE FUNCTION kwabi_guc_test_int(int4)
    RETURNS bool AS :'bundle','kwabi_guc_test_int' LANGUAGE C;
CREATE FUNCTION kwabi_guc_test_string(text)
    RETURNS bool AS :'bundle','kwabi_guc_test_string' LANGUAGE C;
CREATE FUNCTION kwabi_guc_test_bool(bool)
    RETURNS bool AS :'bundle','kwabi_guc_test_bool' LANGUAGE C;
CREATE FUNCTION kwabi_guc_test_float(float8)
    RETURNS bool AS :'bundle','kwabi_guc_test_float' LANGUAGE C;
CREATE FUNCTION kwabi_guc_set_test()
    RETURNS bool AS :'bundle','kwabi_guc_set_test' LANGUAGE C;
CREATE FUNCTION kwabi_guc_control()
    RETURNS bool AS :'bundle','kwabi_guc_control' LANGUAGE C;

\echo ''
\echo '=== 1. read an integer GUC ==='
\echo '   work_mem is a known integer GUC'
SELECT kwabi_guc_test_int(4096) AS int_guc_read;

\echo ''
\echo '=== 2. read a string GUC ==='
\echo '   server_version is a known string GUC'
SELECT kwabi_guc_test_string('PostgreSQL 18.6 on aarch64-apple-darwin') AS string_guc_read;

\echo ''
\echo '=== 3. read a boolean GUC ==='
\echo '   is_superuser is a known boolean GUC'
SELECT kwabi_guc_test_bool(true) AS bool_guc_read;

\echo ''
\echo '=== 4. read a float GUC ==='
\echo '   shared_buffers is a known float GUC (in 8kB units)'
SELECT kwabi_guc_test_float(128) AS float_guc_read;

\echo ''
\echo '=== 5. set and read back GUC values ==='
\echo '   set string, int, bool, float through the ABI, then read them back'
SELECT kwabi_guc_set_test() AS set_and_read_back;

\echo ''
\echo '=== 6. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_guc_set_test() AS in_transaction;
COMMIT;

\echo ''
\echo '=== 7. the backend survived the transaction ==='
SELECT kwabi_guc_test_int(4096) AS after_transaction;

\echo ''
\echo '=== 8. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_guc_control() AS control_should_not_return;

\echo ''
\echo '=== 9. the backend survived the control ==='
SELECT kwabi_guc_test_int(4096) AS after_control;

\echo ''
\echo '=== guc-api tests complete ==='
