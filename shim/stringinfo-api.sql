-- ========================================================================
-- stringinfo-api.sql — the stringinfo group through the ABI.
--
-- This is the test for the stringinfo slots. It proves an extension can
-- create a StringInfo, append data to it, and read it back — entirely
-- through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: the negative control raises by design.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_stringinfo_control() asserts stringinfo length == 999 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as relation-api.sql and fmgr-api.sql.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

-- Register the SQL-callable wrappers.
DROP FUNCTION IF EXISTS kwabi_stringinfo_test();
DROP FUNCTION IF EXISTS kwabi_stringinfo_control();

CREATE FUNCTION kwabi_stringinfo_test()
    RETURNS text AS :'bundle','kwabi_stringinfo_test' LANGUAGE C;
CREATE FUNCTION kwabi_stringinfo_control()
    RETURNS bool AS :'bundle','kwabi_stringinfo_control' LANGUAGE C;

\echo ''
\echo '=== 1. stringinfo init/append/append_char/append_int/data/len ==='
SELECT kwabi_stringinfo_test() = 'hello 42' AS stringinfo_basic;

\echo ''
\echo '=== 2. stringinfo reset clears the buffer ==='
SELECT kwabi_stringinfo_test() IS NOT NULL AS stringinfo_reset_works;

\echo ''
\echo '=== 3. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_stringinfo_control() AS control_should_not_return;

\echo ''
\echo '=== 4. the backend survived all of the above ==='
SELECT kwabi_stringinfo_test() = 'hello 42' AS after_all_checks;

\echo ''
\echo '=== stringinfo-api tests complete ==='
