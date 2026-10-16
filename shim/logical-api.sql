-- Copyright (C) 2026 Simon Janes
-- SPDX-License-Identifier: AGPL-3.0-or-later

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
DROP FUNCTION IF EXISTS kwabi_pgoutput_bytes_test(bytea);
DROP FUNCTION IF EXISTS kwabi_pgoutput_filter_test();
DROP FUNCTION IF EXISTS kwabi_pgoutput_errors_test();
DROP FUNCTION IF EXISTS kwabi_logical_upto_test();
DROP FUNCTION IF EXISTS kwabi_logical_stream_abort_test();
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
CREATE FUNCTION kwabi_logical_stream_abort_test()
    RETURNS bool AS :'bundle','kwabi_logical_stream_abort_test' LANGUAGE C;
CREATE FUNCTION kwabi_pgoutput_bytes_test(bytea)
    RETURNS bool AS :'bundle','kwabi_pgoutput_bytes_test' LANGUAGE C;
CREATE FUNCTION kwabi_pgoutput_filter_test()
    RETURNS bool AS :'bundle','kwabi_pgoutput_filter_test' LANGUAGE C;
CREATE FUNCTION kwabi_pgoutput_errors_test()
    RETURNS bool AS :'bundle','kwabi_pgoutput_errors_test' LANGUAGE C;
CREATE FUNCTION kwabi_logical_upto_test()
    RETURNS bool AS :'bundle','kwabi_logical_upto_test' LANGUAGE C;
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
SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots
    WHERE slot_name = 'kwabi_stream_t';
SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots
    WHERE slot_name = 'kwabi_filter_t';
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

\echo ''
\echo '=== 0b. upto_lsn: a bound at the first COMMIT returns that transaction only, and replays ==='
SELECT kwabi_logical_upto_test() AS logical_upto;

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

\echo ''
\echo '=== 5b. pgoutput bytes equal pg_logical_slot_peek_binary_changes for the same range, message by message ==='
SELECT kwabi_pgoutput_bytes_test((
    SELECT coalesce(string_agg(int4send(length(p.data)) || p.data, ''::bytea ORDER BY p.ord), ''::bytea)
    FROM pg_logical_slot_peek_binary_changes('kwabi_pgo_t', NULL, NULL,
                                             'proto_version', '1', 'publication_names', 'kwabi_pub')
         WITH ORDINALITY AS p(lsn, xid, data, ord)
)) AS pgoutput_bytes;

\echo ''
\echo '=== 5c. a missing publication raises undefined_object; an unknown option raises by name ==='
SELECT kwabi_pgoutput_errors_test() AS pgoutput_errors;

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

\echo ''
-- A streamed transaction for the stream-abort check, in its own table, publication
-- and slot so the checks above keep their counts. logical_decoding_work_mem is
-- lowered for this session, and stays lowered through the check: the decoding
-- backend is this session, so the peek streams only while it is set.
DROP PUBLICATION IF EXISTS kwabi_stream_pub;
DROP TABLE IF EXISTS kwabi_stream_t;
CREATE TABLE kwabi_stream_t (v int);
CREATE PUBLICATION kwabi_stream_pub FOR TABLE kwabi_stream_t;
SELECT pg_create_logical_replication_slot('kwabi_stream_t', 'pgoutput');
SET logical_decoding_work_mem = '64kB';
BEGIN;
INSERT INTO kwabi_stream_t SELECT g FROM generate_series(1, 20000) g;
SAVEPOINT s1;
INSERT INTO kwabi_stream_t SELECT g FROM generate_series(100001, 120000) g;
ROLLBACK TO s1;
INSERT INTO kwabi_stream_t VALUES (100);
COMMIT;

\echo ''
\echo '=== 8b. publication_names filters the stream: only the published table insert appears ==='
DROP PUBLICATION IF EXISTS kwabi_filter_pub;
DROP TABLE IF EXISTS kwabi_filter_t;
DROP TABLE IF EXISTS kwabi_other_t;
CREATE TABLE kwabi_filter_t (v int);
CREATE TABLE kwabi_other_t (v int);
CREATE PUBLICATION kwabi_filter_pub FOR TABLE kwabi_filter_t;
SELECT pg_create_logical_replication_slot('kwabi_filter_t', 'pgoutput');
INSERT INTO kwabi_other_t VALUES (1);
INSERT INTO kwabi_filter_t VALUES (2);
INSERT INTO kwabi_other_t VALUES (3);
SELECT kwabi_pgoutput_filter_test() AS pgoutput_filter;

\echo '=== 9. streamed: a rolled-back savepoint changes come before its A and never after; the rest come after ==='
SELECT kwabi_logical_stream_abort_test() AS logical_stream_abort;
RESET logical_decoding_work_mem;

-- Teardown runs after every test, including the pgoutput and two-phase ones.
SELECT pg_drop_replication_slot('kwabi_stream_t');
SELECT pg_drop_replication_slot('kwabi_filter_t');
DROP PUBLICATION IF EXISTS kwabi_filter_pub;
DROP TABLE IF EXISTS kwabi_filter_t;
DROP TABLE IF EXISTS kwabi_other_t;
DROP PUBLICATION IF EXISTS kwabi_stream_pub;
DROP TABLE IF EXISTS kwabi_stream_t;
SELECT pg_drop_replication_slot('kwabi_2pc_t');
SELECT pg_drop_replication_slot('kwabi_logical_t');
SELECT pg_drop_replication_slot('kwabi_pgo_t');
DROP PUBLICATION IF EXISTS kwabi_pub;
DROP TABLE IF EXISTS kwabi_lg_t;

\echo ''
\echo '=== logical-api tests complete ==='
