-- ========================================================================
-- tuple-api.sql — the tuple/slot group through the ABI.
--
-- This is the test for the tuple/slot kata. It proves an extension can
-- access tuple descriptors, heap tuples, and tuple table slots entirely
-- through the ABI — no direct PostgreSQL calls.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 5 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_tuple_control() asserts a wrong value and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_tuple_test();
DROP FUNCTION IF EXISTS kwabi_heap_tuple_test();
DROP FUNCTION IF EXISTS kwabi_slot_test();
DROP FUNCTION IF EXISTS kwabi_tuple_control();

CREATE FUNCTION kwabi_tuple_test()
    RETURNS bool AS :'bundle','kwabi_tuple_test' LANGUAGE C;
CREATE FUNCTION kwabi_heap_tuple_test()
    RETURNS bool AS :'bundle','kwabi_heap_tuple_test' LANGUAGE C;
CREATE FUNCTION kwabi_slot_test()
    RETURNS bool AS :'bundle','kwabi_slot_test' LANGUAGE C;
CREATE FUNCTION kwabi_tuple_control()
    RETURNS bool AS :'bundle','kwabi_tuple_control' LANGUAGE C;

\echo ''
\echo '=== 1. tuple descriptor accessors ==='
\echo '   tuple_natts, tuple_typeid, tuple_typmod, tuple_attname,'
\echo '   tuple_attisdropped, tuple_attnum'
SELECT kwabi_tuple_test() AS tuple_desc_accessors;

\echo ''
\echo '=== 2. heap tuple accessors ==='
\echo '   heap_tuple_getattr, heap_tuple_setattr, heap_tuple_tableoid,'
\echo '   heap_tuple_tid'
SELECT kwabi_heap_tuple_test() AS heap_tuple_accessors;

\echo ''
\echo '=== 3. slot accessors ==='
\echo '   slot_isnull, slot_getattr, slot_tupledesc'
SELECT kwabi_slot_test() AS slot_accessors;

\echo ''
\echo '=== 4. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_tuple_test() AS in_transaction;
COMMIT;

\echo ''
\echo '=== 5. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_tuple_control() AS control_should_not_return;

\echo ''
\echo '=== 6. the backend survived the control ==='
SELECT kwabi_tuple_test() AS after_control;

\echo ''
\echo '=== tuple-api tests complete ==='
