#!/usr/bin/env bash
# Copyright (C) 2026 Simon Janes
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Verifies the slot guard (slot-guard.sql) on PostgreSQL 18, including the
# idle-timeout invalidation it exists to prevent.
#
#   PGDATA=<cluster dir> PGPORT=<port> [PG_BIN=<pg bin dir>] ./slot-guard-test.sh
#
# The cluster needs wal_level=logical and max_replication_slots > 0. The script
# changes idle_replication_slot_timeout and restores it to 0 on exit. Do not point
# it at a shared cluster.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PG_BIN="${PG_BIN:-$(dirname "$(command -v pg_ctl)")}"
: "${PGDATA:?set PGDATA}"
: "${PGPORT:?set PGPORT}"
export PGHOST=127.0.0.1 PGUSER=postgres PGPORT

psql_q() { "$PG_BIN/psql" -X -q -v ON_ERROR_STOP=1 -At -d postgres "$@"; }

check() {
  local name="$1" want="$2" got="$3"
  if [ "$want" = "$got" ]; then
    echo "PASS $name ($got)"
  else
    echo "FAIL $name: want '$want' got '$got'"
    exit 1
  fi
}

set_timeout() {
  psql_q -c "ALTER SYSTEM SET idle_replication_slot_timeout = $1" >/dev/null
  psql_q -c "SELECT pg_reload_conf()" >/dev/null
  for _ in $(seq 1 50); do
    if [ "$(psql_q -c "SELECT setting FROM pg_settings WHERE name = 'idle_replication_slot_timeout'")" = "$1" ]; then return 0; fi
    sleep 0.1
  done
  echo "FAIL: idle_replication_slot_timeout never became $1"
  exit 1
}

restore() {
  psql_q -c "ALTER SYSTEM RESET idle_replication_slot_timeout" >/dev/null 2>&1 || true
  psql_q -c "SELECT pg_reload_conf()" >/dev/null 2>&1 || true
}
trap restore EXIT

# Leave the slot inactive past the timeout, then checkpoint. This is the engine's
# real pattern: peek acquires and releases the slot, then the consumer waits.
idle_then_checkpoint() {
  local slot="$1"
  psql_q -c "SELECT count(*) FROM pg_logical_slot_peek_changes('$slot', NULL, NULL)" >/dev/null
  sleep 2
  psql_q -c "CHECKPOINT" >/dev/null
}

psql_q -f "$HERE/slot-guard.sql"
psql_q <<'SQL'
SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots WHERE slot_name LIKE 'kwt_guard%';
DELETE FROM kwabi_owned_slot WHERE slot_name LIKE 'kwt_guard%';
SQL

# Scenario 1: with the rule in place, an owned slot passes the guard and survives
# idle-then-checkpoint.
set_timeout 0
psql_q -c "SELECT pg_create_logical_replication_slot('kwt_guard_ok', 'test_decoding')" >/dev/null
psql_q -c "SELECT kwabi_own_slot('kwt_guard_ok')" >/dev/null
check "rule in place: guard passes" "ok" \
  "$(psql_q -c "SELECT kwabi_slot_guard_check('kwt_guard_ok')")"
idle_then_checkpoint kwt_guard_ok
check "rule in place: slot survives idle then checkpoint" "reserved|" \
  "$(psql_q -c "SELECT wal_status || '|' || coalesce(invalidation_reason, '') FROM pg_replication_slots WHERE slot_name='kwt_guard_ok'")"

# Scenario 2: the guard refuses when the timeout is non-zero for an owned slot.
set_timeout 1
err=$(psql_q -c "SELECT kwabi_slot_guard_check('kwt_guard_ok')" 2>&1 || true)
check "timeout set: guard reports an error" "1" \
  "$(grep -c 'must be 0' <<<"$err")"

# Scenario 3: a slot the engine does not own is not checked, and is refused as unowned.
psql_q -c "SELECT pg_create_logical_replication_slot('kwt_guard_other', 'test_decoding')" >/dev/null
err=$(psql_q -c "SELECT kwabi_slot_guard_check('kwt_guard_other')" 2>&1 || true)
check "unowned slot: guard refuses" "1" \
  "$(grep -c 'not engine-owned' <<<"$err")"

# Scenario 4: negative control. With the timeout non-zero, the same idle-then-checkpoint
# invalidates the slot, so scenario 1 is testing something real.
psql_q -c "SELECT pg_create_logical_replication_slot('kwt_guard_bad', 'test_decoding')" >/dev/null
idle_then_checkpoint kwt_guard_bad
check "control: timeout 1s invalidates an idle slot" "lost|idle_timeout" \
  "$(psql_q -c "SELECT wal_status || '|' || coalesce(invalidation_reason, '') FROM pg_replication_slots WHERE slot_name='kwt_guard_bad'")"

# Scenario 5: with the rule back in place, a slot the server already lost reports lost.
set_timeout 0
psql_q -c "SELECT kwabi_own_slot('kwt_guard_bad')" >/dev/null
check "lost slot: guard reports lost" "lost" \
  "$(psql_q -c "SELECT kwabi_slot_guard_check('kwt_guard_bad')")"

psql_q -c "SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots WHERE slot_name LIKE 'kwt_guard%'" >/dev/null
echo "all slot guard checks passed"
