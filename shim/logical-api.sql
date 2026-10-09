-- ========================================================================
-- logical-api.sql — logical decoding through the ABI.
--
-- Runs on a throwaway cluster with wal_level = logical (ci-local.sh starts it).
-- The fixture is committed by these statements before the C tests run: each
-- INSERT is its own transaction, so the decoded stream has two BEGIN/COMMIT
-- pairs, each with one INSERT. A decode cannot see uncommitted rows, so the
-- tests cannot create their own data.
--
-- Every check is an explicit boolean. The harness counts `t` and `f`.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_logical_read_test();
DROP FUNCTION IF EXISTS kwabi_logical_after_confirm_test();
DROP FUNCTION IF EXISTS kwabi_logical_missing_slot_test();

CREATE FUNCTION kwabi_logical_read_test()
    RETURNS bool AS :'bundle','kwabi_logical_read_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_after_confirm_test()
    RETURNS bool AS :'bundle','kwabi_logical_after_confirm_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_missing_slot_test()
    RETURNS bool AS :'bundle','kwabi_logical_missing_slot_test' LANGUAGE C;

SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots
    WHERE slot_name = 'kwabi_logical_t';
DROP TABLE IF EXISTS kwabi_lg_t;
CREATE TABLE kwabi_lg_t (v int4);
SELECT pg_create_logical_replication_slot('kwabi_logical_t', 'test_decoding');
INSERT INTO kwabi_lg_t VALUES (1);
INSERT INTO kwabi_lg_t VALUES (2);

\echo ''
\echo '=== 1. read: two committed inserts, with xids; confirm past the end raises; confirm to the end works ==='
SELECT kwabi_logical_read_test() AS logical_read;

\echo ''
\echo '=== 2. after confirm: a new handle sees no inserts (the slot advanced) ==='
SELECT kwabi_logical_after_confirm_test() AS logical_after_confirm;

\echo ''
\echo '=== 3. begin on a missing slot raises ==='
SELECT kwabi_logical_missing_slot_test() AS logical_missing_slot;

SELECT pg_drop_replication_slot('kwabi_logical_t');
DROP TABLE IF EXISTS kwabi_lg_t;

\echo ''
\echo '=== logical-api tests complete ==='
