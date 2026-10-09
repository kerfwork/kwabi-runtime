#!/usr/bin/env bash
# Reloadable hook bodies through the runtime's name table (design section 4), on a
# preloaded cluster for one PostgreSQL major.
#
#   SCRATCH=<dir> ./hook-reload-run.sh <major>
#
# Builds the bundle with `make build PG=<major>` first. Builds two versions of the body
# (reload/reload_body.c, tags "1" and "2") into SCRATCH. Session B is a backend that is
# already running when the body is bound, so it must switch on its next statement.
set -euo pipefail
cd "$(dirname "$0")"
major="${1:?major}"
: "${SCRATCH:?set SCRATCH}"

# PGBIN overrides the PostgreSQL bin directory (ci-local passes it); DLSUFFIX is the
# platform's shared-library suffix; HR_PORT overrides the default port.
bin="${PGBIN:-/opt/homebrew/opt/postgresql@$major/bin}"
dl="${DLSUFFIX:-dylib}"
inc=$PWD/../vendor/kwabi/include
bundle="$PWD/kwabi_runtime_pg$major.$dl"
port="${HR_PORT:-$((5900 + major))}"
data="$SCRATCH/hook-reload-pg$major"
v1="$SCRATCH/reload_body_v1.dylib"
v2="$SCRATCH/reload_body_v2.dylib"
v2b="$SCRATCH/reload_body_v2_fresh.dylib"
fail=0

check() {
  if [ "$2" = "$3" ]; then echo "  PASS $1 ($3)"; else echo "  FAIL $1: want '$2' got '$3'"; fail=1; fi
}

cc -shared -fPIC -Wall -I"$inc" -DBODY_TAG='"1"' -o "$v1" reload/reload_body.c
cc -shared -fPIC -Wall -I"$inc" -DBODY_TAG='"2"' -o "$v2" reload/reload_body.c
cp "$v2" "$v2b"

"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true
rm -rf "$data"
"$bin/initdb" -D "$data" -U postgres -A trust >"$SCRATCH/hook-reload-initdb-$major.log" 2>&1
cat >> "$data/postgresql.conf" <<CONF
port = $port
listen_addresses = '127.0.0.1'
unix_socket_directories = ''
shared_preload_libraries = '$bundle'
CONF
"$bin/pg_ctl" -D "$data" -l "$SCRATCH/hook-reload-server-$major.log" -w start >/dev/null
trap '"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true' EXIT

# Value on stdout; warnings and errors go to a log, so a check compares only the value.
A() { "$bin/psql" -X -q -At -h 127.0.0.1 -p $port -U postgres -d postgres -c "$1" 2>>"$SCRATCH/hook-reload-A-$major.stderr"; }

"$bin/psql" -X -q -h 127.0.0.1 -p $port -U postgres -d postgres -v ON_ERROR_STOP=1 <<SQL
CREATE FUNCTION kwabi_hook_test_bind(text, text) RETURNS text AS '$bundle', 'kwabi_hook_test_bind' LANGUAGE C STRICT;
CREATE FUNCTION kwabi_capabilities() RETURNS bigint AS '$bundle', 'kwabi_capabilities' LANGUAGE C STRICT;
SQL

# Session B: a persistent backend fed from a FIFO, started before any bind.
fifo="$SCRATCH/hook-reload-B-$major.in"; out="$SCRATCH/hook-reload-B-$major.out"
rm -f "$fifo" "$out"; mkfifo "$fifo"
"$bin/psql" -X -q -At -h 127.0.0.1 -p $port -U postgres -d postgres < "$fifo" > "$out" 2>&1 &
exec 3> "$fifo"
mark=0
B() {
  mark=$((mark+1)); local before=$mark
  printf '%s\n\\echo MARK%s\n' "$1" "$mark" >&3
  for _ in $(seq 1 400); do grep -q "^MARK$mark\$" "$out" 2>/dev/null && break; sleep 0.05; done
  LASTOUT=$(awk -v m="MARK$mark" -v p="MARK$((before-1))" 'BEGIN{on=(p=="MARK0")} $0==p {on=1; next} $0==m {exit} on && $0!="" {print}' "$out")
}
# Run in B, then return the error text or the value of the statement.
BV() { B "$1"; RESULT=$(printf '%s\n' "$LASTOUT" | tail -1); }

check "capability bit set when preloaded" "t" "$(A "SELECT (kwabi_capabilities() & 128) <> 0")"
BV "SELECT count(*) FROM generate_series(1,1) /* kwt_reload */;"
check "before any bind, nothing refuses" "1" "$RESULT"

check "bind name to v1" "bound" "$(A "SELECT kwabi_hook_test_bind('kwt_ext', '$v1')")"
BV "SELECT count(*) FROM generate_series(1,1) /* kwt_reload */;"
check "running session B picks up v1 on its next statement" "reload body 1 refused" "$(printf '%s\n' "$LASTOUT" | grep -o 'reload body [0-9]* refused' | head -1)"
BV "SELECT count(*) FROM generate_series(1,1);"
check "unmarked statements still run" "1" "$RESULT"

check "rebind name to v2" "bound" "$(A "SELECT kwabi_hook_test_bind('kwt_ext', '$v2')")"
BV "SELECT count(*) FROM generate_series(1,1) /* kwt_reload */;"
check "session B switches to v2 without restart" "reload body 2 refused" "$(printf '%s\n' "$LASTOUT" | grep -o 'reload body [0-9]* refused' | head -1)"

check "bind records a missing path without failing" "bound" "$(A "SELECT kwabi_hook_test_bind('kwt_ext', '$SCRATCH/missing.dylib')")"
BV "SELECT count(*) FROM generate_series(1,1) /* kwt_reload */;"
check "bad path keeps the last good body" "reload body 2 refused" "$(printf '%s\n' "$LASTOUT" | grep -o 'reload body [0-9]* refused' | head -1)"

check "over-long name is refused" "bind refused" "$(A "SELECT kwabi_hook_test_bind('$(printf 'x%.0s' $(seq 1 80))', '$v1')")"

exec 3>&-
if [ "$fail" -eq 0 ]; then echo "hook reload checks passed (PG $major)"; else echo "FAILURES (PG $major)"; exit 1; fi
