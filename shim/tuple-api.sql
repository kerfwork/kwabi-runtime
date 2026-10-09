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
DROP FUNCTION IF EXISTS kwabi_slot_is_active_test();
DROP FUNCTION IF EXISTS kwabi_slot_get_lsn_test();
DROP FUNCTION IF EXISTS kwabi_slot_get_catalog_xmin_test();
DROP FUNCTION IF EXISTS kwabi_itempointer_get_block_number_test();
DROP FUNCTION IF EXISTS kwabi_tuple_control();
DROP FUNCTION IF EXISTS kwabi_itempointer_is_valid_test();
DROP FUNCTION IF EXISTS kwabi_itempointer_get_offset_number_test();
DROP FUNCTION IF EXISTS kwabi_block_get_number_test();
DROP FUNCTION IF EXISTS kwabi_block_is_valid_test();
DROP FUNCTION IF EXISTS kwabi_block_get_offset_test();

CREATE FUNCTION kwabi_tuple_test()
    RETURNS bool AS :'bundle','kwabi_tuple_test' LANGUAGE C;
CREATE FUNCTION kwabi_heap_tuple_test()
    RETURNS bool AS :'bundle','kwabi_heap_tuple_test' LANGUAGE C;
CREATE FUNCTION kwabi_slot_test()
    RETURNS bool AS :'bundle','kwabi_slot_test' LANGUAGE C;
CREATE FUNCTION kwabi_slot_is_active_test()
    RETURNS bool AS :'bundle','kwabi_slot_is_active_test' LANGUAGE C;
CREATE FUNCTION kwabi_slot_get_lsn_test()
    RETURNS bool AS :'bundle','kwabi_slot_get_lsn_test' LANGUAGE C;
CREATE FUNCTION kwabi_slot_get_catalog_xmin_test()
    RETURNS bool AS :'bundle','kwabi_slot_get_catalog_xmin_test' LANGUAGE C;
CREATE FUNCTION kwabi_itempointer_get_block_number_test()
    RETURNS bool AS :'bundle','kwabi_itempointer_get_block_number_test' LANGUAGE C;
CREATE FUNCTION kwabi_itempointer_is_valid_test()
    RETURNS bool AS :'bundle','kwabi_itempointer_is_valid_test' LANGUAGE C;
CREATE FUNCTION kwabi_itempointer_get_offset_number_test()
    RETURNS bool AS :'bundle','kwabi_itempointer_get_offset_number_test' LANGUAGE C;
CREATE FUNCTION kwabi_block_get_number_test()
    RETURNS bool AS :'bundle','kwabi_block_get_number_test' LANGUAGE C;
CREATE FUNCTION kwabi_block_get_offset_test()
    RETURNS bool AS :'bundle','kwabi_block_get_offset_test' LANGUAGE C;
CREATE FUNCTION kwabi_block_is_valid_test()
    RETURNS bool AS :'bundle','kwabi_block_is_valid_test' LANGUAGE C;
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
\echo '=== 3b. slot_is_active ==='
SELECT kwabi_slot_is_active_test() AS slot_is_active;

\echo ''
\echo '=== 3c. slot_get_lsn ==='
SELECT kwabi_slot_get_lsn_test() AS slot_get_lsn;

\echo ''
\echo '=== 3d. slot_get_catalog_xmin ==='
SELECT kwabi_slot_get_catalog_xmin_test() AS slot_get_catalog_xmin;

\echo ''
\echo '=== 3e. itempointer_get_block_number ==='
SELECT kwabi_itempointer_get_block_number_test() AS itempointer_get_block_number;

\echo ''
\echo '=== 3f. itempointer_is_valid ==='
SELECT kwabi_itempointer_is_valid_test() AS itempointer_is_valid;

\echo ''
\echo '=== 3g. itempointer_get_offset_number ==='
SELECT kwabi_itempointer_get_offset_number_test() AS itempointer_get_offset_number;

\echo ''
\echo '=== 3h. block_get_number ==='
SELECT kwabi_block_get_number_test() AS block_get_number;

\echo ''
\echo '=== 3i. block_get_offset ==='
SELECT kwabi_block_get_offset_test() AS block_get_offset;

\echo ''
\echo '=== 3i. block_is_valid ==='
SELECT kwabi_block_is_valid_test() AS block_is_valid;

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
