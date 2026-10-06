-- ========================================================================
-- lock-api.sql — the lock group through the ABI.
--
-- This is the test for the lock slots. It proves an extension can
-- acquire, release, and check locks — entirely through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 5 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_lock_control() asserts lock_held_by_me returns false after acquire
--     and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_lock_test();
DROP FUNCTION IF EXISTS kwabi_spin_test();
DROP FUNCTION IF EXISTS kwabi_spinlock_test();
DROP FUNCTION IF EXISTS kwabi_lock_control();

CREATE FUNCTION kwabi_lock_test()
    RETURNS bool AS :'bundle','kwabi_lock_test' LANGUAGE C;
CREATE FUNCTION kwabi_spin_test()
    RETURNS bool AS :'bundle','kwabi_spin_test' LANGUAGE C;
CREATE FUNCTION kwabi_spinlock_test()
    RETURNS bool AS :'bundle','kwabi_spinlock_test' LANGUAGE C;
CREATE FUNCTION kwabi_lock_control()
    RETURNS bool AS :'bundle','kwabi_lock_control' LANGUAGE C;

\echo ''
\echo '=== 1. lock_acquire + lock_held_by_me + lock_release ==='
\echo '   LWLock must be acquirable, held, and releasable'
SELECT kwabi_lock_test() AS lock_lifecycle;

\echo ''
\echo '=== 2. spin_acquire + spin_release ==='
\echo '   SpinLock must be acquirable and releasable'
SELECT kwabi_spin_test() AS spin_lifecycle;

\echo ''
\echo '=== 3. spinlock_acquire + spinlock_release + spinlock_held_by_me ==='
\echo '   SpinLock (via spinlock slots) must be acquirable and releasable'
SELECT kwabi_spinlock_test() AS spinlock_lifecycle;

\echo ''
\echo '=== 4. the backend survived all of the above ==='
SELECT kwabi_lock_test() AS after_all_checks;

\echo ''
\echo '=== 5. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_lock_control() AS control_should_not_return;

\echo ''
\echo '=== lock-api tests complete ==='
