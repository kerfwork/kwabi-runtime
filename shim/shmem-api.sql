-- ========================================================================
-- shmem-api.sql — the shmem group through the ABI.
--
-- This is the test for the shmem slots. It proves an extension can
-- allocate shared memory, write to it, read it back, and free it — entirely
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
--   * kwabi_shmem_control() asserts sizeof(void*) == 999 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as relation-api.sql and fmgr-api.sql.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

-- Register the SQL-callable wrappers.
DROP FUNCTION IF EXISTS kwabi_shmem_test();
DROP FUNCTION IF EXISTS kwabi_shmem_control();

CREATE FUNCTION kwabi_shmem_test()
    RETURNS text AS :'bundle','kwabi_shmem_test' LANGUAGE C;
CREATE FUNCTION kwabi_shmem_control()
    RETURNS bool AS :'bundle','kwabi_shmem_control' LANGUAGE C;

\echo ''
\echo '=== 1. shmem alloc/write/read/free ==='
SELECT kwabi_shmem_test() = 'kwabi: shmem test passed' AS shmem_basic;

\echo ''
\echo '=== 2. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_shmem_control() AS control_should_not_return;

\echo ''
\echo '=== 3. the backend survived all of the above ==='
SELECT kwabi_shmem_test() = 'kwabi: shmem test passed' AS after_all_checks;

\echo ''
\echo '=== shmem-api tests complete ==='
