-- ========================================================================
-- subxact-probe.sql — the two-way control for fmgr-api check 13.
--
-- check 13 asserts that a failed call through a call_function slot leaves NO
-- partial work. That assertion is only evidence if it FAILS when the
-- subtransaction is removed, so this probe is the demonstration:
--
--   ./subxact-probe.sh            # builds the stripped variant and runs both
--
-- or by hand:
--   1. the shipped bundle       -> after_rows = 0  (rollback worked)
--   2. a bundle built with the BeginInternalSubTransaction / Release /
--      RollbackAndRelease calls removed from shim_call_impl
--                               -> after_rows = 1  (write survived)
--
-- The EXPLICIT transaction is what makes this discriminate. In autocommit the
-- error aborts the implicit transaction and discards the write anyway, so the
-- count would be 0 with or without the subtransaction -- a control that cannot
-- fail. Measured: autocommit 0/0, explicit txn 0/1.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS probe_ins_raise_call(int4, int4);
CREATE FUNCTION probe_ins_raise_call(int4, int4)
    RETURNS text AS :'bundle','kwabi_fmgr_ins_then_raise' LANGUAGE C;

-- Fresh table every run: a survivor from a previous run (e.g. the stripped
-- control) would otherwise be counted and make the result depend on history.
DROP TABLE IF EXISTS probe_t;
CREATE TABLE probe_t(tag int);

DROP FUNCTION IF EXISTS probe_ins_raise(int4);
CREATE FUNCTION probe_ins_raise(int4) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO probe_t VALUES ($1);
    RAISE EXCEPTION 'boom';
END $$;

BEGIN;
SELECT probe_ins_raise_call(
    (SELECT oid::int4 FROM pg_proc WHERE proname = 'probe_ins_raise'),
    4242) AS sqlstate;   -- must be P0001: the error was caught
SELECT count(*) AS after_rows FROM probe_t;   -- 0 = rolled back, 1 = survived
ROLLBACK;
