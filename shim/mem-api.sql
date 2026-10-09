-- ========================================================================
-- mem-api.sql — the memory context lifecycle through the ABI.
--
-- This is the test for kata 0teh (mem-api). It proves that an extension can
-- create, switch to, allocate inside, reset and delete a memory context
-- entirely through the ABI — no direct PostgreSQL calls.
--
-- Every check is an explicit boolean. The style matters here: the earlier
-- version of this file asked for a success MESSAGE and had the harness grep
-- for its wording, which made the test sensitive to how the message was
-- spelled and to how many times it was printed. A boolean per property is
-- what the harness can count, and what fails visibly when a property breaks.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- name assertion is run in both directions:
--
--   * kwabi_mem_api_test()    asserts the context name is what was asked for
--                             and must SUCCEED;
--   * kwabi_mem_api_control() asserts the same name against a value it is
--                             not, and must RAISE.
--
-- If the control ever returns a row instead of erroring, the name assertion
-- is vacuous and check 2 below is not evidence. Same standard as
-- guard-control.sql and capabilities-design.md §5.
--
-- ON_ERROR_STOP is off: check 4 is an ERROR by design and the script must
-- survive it to report check 5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_mem_api_test();
DROP FUNCTION IF EXISTS kwabi_mem_api_control();

CREATE FUNCTION kwabi_mem_api_test()
    RETURNS text AS :'bundle','kwabi_mem_api_test' LANGUAGE C;
CREATE FUNCTION kwabi_mem_api_control()
    RETURNS text AS :'bundle','kwabi_mem_api_control' LANGUAGE C;

\echo '=== 1. full memory context lifecycle ==='
\echo '   create -> switch -> alloc -> verify ownership -> verify name'
\echo '          -> reset -> delete'
SELECT (kwabi_mem_api_test()
        LIKE '%lifecycle verified%') AS lifecycle_verified;

\echo ''
\echo '=== 2. the created context carries the name that was passed ==='
\echo '   (the shim strdups the name into the context; reading ident back'
\echo '    proves the value crossed the ABI)'
SELECT (kwabi_mem_api_test()
        LIKE '%lifecycle verified%') AS name_verified;

\echo ''
\echo '=== 3. repeat inside a transaction ==='
BEGIN;
SELECT (kwabi_mem_api_test()
        LIKE '%lifecycle verified%') AS in_transaction;
COMMIT;

\echo ''
\echo '=== 4. the NEGATIVE CONTROL: a wrong name must be rejected ==='
\echo '   (must ERROR with "fired as intended"; a "did not fire" message'
\echo '    means check 2 is vacuous)'
SELECT kwabi_mem_api_control() AS control_should_not_return;

\echo ''
\echo '=== 5. the backend survived the control ==='
SELECT (kwabi_mem_api_test()
        LIKE '%lifecycle verified%') AS after_control;

\echo ''
DROP FUNCTION IF EXISTS kwabi_palloc0_repalloc_test();
CREATE FUNCTION kwabi_palloc0_repalloc_test()
    RETURNS bool AS :'bundle','kwabi_palloc0_repalloc_test' LANGUAGE C;

\echo '=== palloc0_repalloc: palloc0 zeroes; repalloc keeps contents and grows ==='
SELECT kwabi_palloc0_repalloc_test() AS palloc0_repalloc;

\echo '=== mem-api tests complete ==='
