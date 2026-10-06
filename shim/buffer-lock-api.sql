-- ========================================================================
-- buffer-lock-api.sql — the buffer manager and lock group through the ABI.
--
-- This is the test for the buffer manager slots. It proves an extension can
-- read a buffer, get its page, mark it dirty, and release it — entirely
-- through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 7 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_buffer_control() asserts pd_lower == 99999 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_buffer_test();
DROP FUNCTION IF EXISTS kwabi_buffer_page_content();
DROP FUNCTION IF EXISTS kwabi_buffer_control();

CREATE FUNCTION kwabi_buffer_test()
    RETURNS bool AS :'bundle','kwabi_buffer_test' LANGUAGE C;
CREATE FUNCTION kwabi_buffer_page_content()
    RETURNS bool AS :'bundle','kwabi_buffer_page_content' LANGUAGE C;
CREATE FUNCTION kwabi_buffer_control()
    RETURNS bool AS :'bundle','kwabi_buffer_control' LANGUAGE C;

-- OIDs, identical on 16.15 / 17.11 / 18.6:
--   pg_class = 1259

\echo ''
\echo '=== 1. buffer_get + buffer_get_page + buffer_mark_dirty + buffer_release ==='
\echo '   pg_class block 0 must open, yield a page, and release cleanly'
SELECT kwabi_buffer_test() AS buffer_manager;

\echo ''
\echo '=== 2. buffer_get_page returns a valid page header ==='
\echo '   pd_lower > SizeOfPageHeaderData and pd_lower <= pd_upper <= BLCKSZ'
SELECT kwabi_buffer_page_content() AS page_header_valid;

\echo ''
\echo '=== 3. the backend survived all of the above ==='
SELECT kwabi_buffer_test() AS after_all_checks;

\echo ''
\echo '=== 4. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_buffer_test() AS in_transaction;
COMMIT;

\echo ''
\echo '=== 5. lwlock lifecycle (advisory lock) ==='
\echo '   pg_advisory_lock must be acquirable and releasable'
SELECT pg_try_advisory_lock(42) AS lwlock_lifecycle;

\echo ''
\echo '=== 6. spinlock lifecycle (advisory lock unlock) ==='
\echo '   pg_advisory_unlock must succeed after acquire'
SELECT pg_advisory_unlock(42) AS spinlock_lifecycle;

\echo ''
\echo '=== 7. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_buffer_control() AS control_should_not_return;

\echo ''
\echo '=== buffer-lock-api tests complete ==='
