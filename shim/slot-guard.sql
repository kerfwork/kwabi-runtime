-- Copyright (C) 2026 Simon Janes
-- SPDX-License-Identifier: AGPL-3.0-or-later

-- Slot guard for engine-owned logical slots (engine-design.md §3.4, §12 Q7).
--
-- PostgreSQL 18 invalidates a slot as idle_timeout when it is left inactive past
-- idle_replication_slot_timeout and then checkpointed. The engine's own peek
-- acquires and releases the slot, so between batches it looks inactive. The rule
-- is that engine-owned slots run with idle_replication_slot_timeout = 0.
--
-- The guard observes and reports. It never changes a setting or a slot. It raises
-- an error when the timeout is non-zero for a slot the engine owns, so a
-- misconfigured server fails loudly instead of losing a slot at a checkpoint.

CREATE TABLE IF NOT EXISTS kwabi_owned_slot (
    slot_name  name         PRIMARY KEY,
    owned_at   timestamptz  NOT NULL DEFAULT now()
);

-- Register a slot as engine-owned. Idempotent.
CREATE OR REPLACE FUNCTION kwabi_own_slot(p_slot name) RETURNS void
LANGUAGE sql AS $$
    INSERT INTO kwabi_owned_slot (slot_name) VALUES (p_slot)
    ON CONFLICT (slot_name) DO NOTHING
$$;

-- Returns 'ok', or 'lost' when the server has already invalidated the slot (the
-- consumer must resync, §7.2). Raises an error when the slot is not owned, when
-- the slot does not exist, or when idle_replication_slot_timeout is non-zero.
CREATE OR REPLACE FUNCTION kwabi_slot_guard_check(p_slot name) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    v_timeout  int;
    v_status   text;
BEGIN
    IF NOT EXISTS (SELECT 1 FROM kwabi_owned_slot WHERE slot_name = p_slot) THEN
        RAISE EXCEPTION 'slot "%" is not engine-owned', p_slot;
    END IF;

    -- pg_settings.setting is the raw integer; current_setting() adds the unit ("1s").
    SELECT setting::int INTO v_timeout FROM pg_settings WHERE name = 'idle_replication_slot_timeout';
    IF v_timeout <> 0 THEN
        RAISE EXCEPTION 'idle_replication_slot_timeout = % seconds would invalidate engine-owned slot "%"; it must be 0',
            v_timeout, p_slot
            USING HINT = 'engine-design.md §12 Q7: the engine peek leaves the slot inactive between batches.';
    END IF;

    SELECT wal_status INTO v_status FROM pg_replication_slots WHERE slot_name = p_slot;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'slot "%" does not exist', p_slot;
    END IF;

    IF v_status = 'lost' THEN
        RETURN 'lost';
    END IF;
    RETURN 'ok';
END;
$$;
