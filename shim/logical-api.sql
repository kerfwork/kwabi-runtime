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

DROP FUNCTION IF EXISTS kwabi_logical_batch_test();
DROP FUNCTION IF EXISTS kwabi_pgoutput_test();
DROP FUNCTION IF EXISTS kwabi_2pc_prepared_test();
DROP FUNCTION IF EXISTS kwabi_2pc_commit_test();
DROP FUNCTION IF EXISTS kwabi_2pc_rollback_test();
DROP FUNCTION IF EXISTS kwabi_pgoutput_no_options_test();
DROP FUNCTION IF EXISTS kwabi_logical_read_test();
DROP FUNCTION IF EXISTS kwabi_logical_after_confirm_test();
DROP FUNCTION IF EXISTS kwabi_logical_missing_slot_test();

CREATE FUNCTION kwabi_2pc_prepared_test()
    RETURNS bool AS :'bundle','kwabi_2pc_prepared_test' LANGUAGE C;
CREATE FUNCTION kwabi_2pc_commit_test()
    RETURNS bool AS :'bundle','kwabi_2pc_commit_test' LANGUAGE C;
CREATE FUNCTION kwabi_2pc_rollback_test()
    RETURNS bool AS :'bundle','kwabi_2pc_rollback_test' LANGUAGE C;
CREATE FUNCTION kwabi_pgoutput_test()
    RETURNS bool AS :'bundle','kwabi_pgoutput_test' LANGUAGE C;
CREATE FUNCTION kwabi_pgoutput_no_options_test()
    RETURNS bool AS :'bundle','kwabi_pgoutput_no_options_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_batch_test()
    RETURNS bool AS :'bundle','kwabi_logical_batch_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_read_test()
    RETURNS bool AS :'bundle','kwabi_logical_read_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_after_confirm_test()
    RETURNS bool AS :'bundle','kwabi_logical_after_confirm_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_missing_slot_test()
    RETURNS bool AS :'bundle','kwabi_logical_missing_slot_test' LANGUAGE C;

SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots
    WHERE slot_name = 'kwabi_logical_t';
SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots
    WHERE slot_name = 'kwabi_pgo_t';
SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots
    WHERE slot_name = 'kwabi_2pc_t';
DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM pg_prepared_xacts WHERE gid IN ('kwabi_2pc', 'kwabi_2pc_rb')) THEN
        EXECUTE 'ROLLBACK PREPARED ''kwabi_2pc''';
    END IF;
END $$;
DROP PUBLICATION IF EXISTS kwabi_pub;
DROP TABLE IF EXISTS kwabi_lg_t;
CREATE TABLE kwabi_lg_t (v int4);
CREATE PUBLICATION kwabi_pub FOR TABLE kwabi_lg_t;
SELECT pg_create_logical_replication_slot('kwabi_logical_t', 'test_decoding');
SELECT pg_create_logical_replication_slot('kwabi_pgo_t', 'pgoutput');
INSERT INTO kwabi_lg_t VALUES (1);
INSERT INTO kwabi_lg_t VALUES (2);

\echo ''
\echo ''
\echo '=== 0. a capped batch returns the cap, and an unconfirmed slot replays it ==='
SELECT kwabi_logical_batch_test() AS logical_batch;

\echo '=== 1. read: two committed inserts, with xids; confirm past the end raises; confirm to the end works ==='
SELECT kwabi_logical_read_test() AS logical_read;

\echo ''
\echo '=== 2. after confirm: a new handle sees no inserts (the slot advanced) ==='
SELECT kwabi_logical_after_confirm_test() AS logical_after_confirm;

\echo ''
\echo '=== 3. begin on a missing slot raises ==='
SELECT kwabi_logical_missing_slot_test() AS logical_missing_slot;


\echo ''
\echo '=== 4. pgoutput through the same begin: binary messages with the publication options ==='
SELECT kwabi_pgoutput_test() AS pgoutput_messages;

\echo ''
\echo '=== 5. pgoutput without its options raises ==='
SELECT kwabi_pgoutput_no_options_test() AS pgoutput_no_options;

-- Two-phase: the slot must be created with twophase before the prepared transaction.
-- Each statement is its own transaction except the BEGIN block below, which is
-- the prepared one. Order matters: prepare, commit, then prepare and roll back.
SELECT pg_create_logical_replication_slot('kwabi_2pc_t', 'pgoutput', false, true);
BEGIN;
INSERT INTO kwabi_lg_t VALUES (7);
PREPARE TRANSACTION 'kwabi_2pc';
\echo ''
\echo '=== 6. two-phase: after PREPARE, the data is delivered (b, I, P) and nothing is committed ==='
SELECT kwabi_2pc_prepared_test() AS twophase_prepared;

COMMIT PREPARED 'kwabi_2pc';
\echo ''
\echo '=== 7. two-phase: COMMIT PREPARED arrives as K, not C; the read is confirmed ==='
SELECT kwabi_2pc_commit_test() AS twophase_commit;

BEGIN;
INSERT INTO kwabi_lg_t VALUES (8);
PREPARE TRANSACTION 'kwabi_2pc_rb';
ROLLBACK PREPARED 'kwabi_2pc_rb';
\echo ''
\echo '=== 8. two-phase: ROLLBACK PREPARED arrives as r, after the confirmed position ==='
SELECT kwabi_2pc_rollback_test() AS twophase_rollback;

-- Teardown runs after every test, including the pgoutput and two-phase ones.
SELECT pg_drop_replication_slot('kwabi_2pc_t');
SELECT pg_drop_replication_slot('kwabi_logical_t');
SELECT pg_drop_replication_slot('kwabi_pgo_t');
DROP PUBLICATION IF EXISTS kwabi_pub;
DROP TABLE IF EXISTS kwabi_lg_t;

\echo ''
\echo '=== logical-api tests complete ==='
