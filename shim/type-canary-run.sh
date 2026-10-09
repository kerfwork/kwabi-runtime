#!/usr/bin/env bash
# The uint64 canary: a SQL type whose text I/O is a reloadable body (name-binding).
#
#   SCRATCH=<dir> ./type-canary-run.sh <major>
#
# Builds the bundle first (make build PG=<major>). Runs a preloaded cluster, creates the
# type through the runtime's generic I/O functions, and checks round-trip, errors (each
# with a control), reload to a new body, and an unbound type name.
set -euo pipefail
cd "$(dirname "$0")"
major="${1:?major}"
: "${SCRATCH:?set SCRATCH}"

bin="${PGBIN:-/opt/homebrew/opt/postgresql@$major/bin}"
dl="${DLSUFFIX:-dylib}"
bundle="$PWD/kwabi_runtime_pg$major.$dl"
port="${CANARY_PORT:-$((5950 + major))}"
data="$SCRATCH/type-canary-pg$major"
v1="$SCRATCH/uint_body_v1.$dl"
v2="$SCRATCH/uint_body_v2.$dl"
inc=$PWD/../vendor/kwabi/include
fail=0

check() {
  if [ "$2" = "$3" ]; then echo "  PASS $1 ($3)"; else echo "  FAIL $1: want '$2' got '$3'"; fail=1; fi
}

cc -shared -fPIC -Wall -Wextra -I"$inc" -DOUT_PREFIX='""' -o "$v1" canary-uint/uint_body.c
cc -shared -fPIC -Wall -Wextra -I"$inc" -DOUT_PREFIX='"u"' -o "$v2" canary-uint/uint_body.c

"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true
rm -rf "$data"
"$bin/initdb" -D "$data" -U postgres -A trust >"$SCRATCH/type-canary-initdb-$major.log" 2>&1
cat >> "$data/postgresql.conf" <<CONF
port = $port
listen_addresses = '127.0.0.1'
unix_socket_directories = ''
shared_preload_libraries = '$bundle'
CONF
"$bin/pg_ctl" -D "$data" -l "$SCRATCH/type-canary-server-$major.log" -w start >/dev/null
trap '"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true' EXIT

Q() { "$bin/psql" -X -q -At -h 127.0.0.1 -p $port -U postgres -d postgres -c "$1" 2>>"$SCRATCH/type-canary-$major.stderr"; }
# An error's SQLSTATE, or "ok", for a statement; the value is not needed.
STATE() {
  "$bin/psql" -X -q -At -h 127.0.0.1 -p $port -U postgres -d postgres -v ON_ERROR_STOP=1 \
    -c "DO \$\$ BEGIN EXECUTE \$q\$$1\$q\$; RAISE NOTICE 'state=ok'; EXCEPTION WHEN others THEN RAISE NOTICE 'state=%', SQLSTATE; END \$\$;" 2>&1 \
    | sed -n 's/.*state=//p'
}

# The bundle's generic I/O, then the type. The binding is "uint64": its name minus _in/_out.
"$bin/psql" -X -q -h 127.0.0.1 -p $port -U postgres -d postgres -v ON_ERROR_STOP=1 <<SQL
CREATE TYPE uint64;
CREATE FUNCTION uint64_in(cstring) RETURNS uint64 AS '$bundle', 'kwabi_type_in' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION uint64_out(uint64) RETURNS cstring AS '$bundle', 'kwabi_type_out' LANGUAGE C IMMUTABLE STRICT;
CREATE TYPE uint64 (INPUT = uint64_in, OUTPUT = uint64_out, INTERNALLENGTH = 8,
                    PASSEDBYVALUE, ALIGNMENT = double);
CREATE TABLE tu (id int, v uint64);
SQL

check "before any bind, the type reports it is unbound" "XX000" "$(STATE "SELECT '1'::uint64")"

Q "SELECT 1" >/dev/null
# Bind the name to v1. The bind is the reload point.
"$bin/psql" -X -q -At -h 127.0.0.1 -p $port -U postgres -d postgres -c "CREATE FUNCTION kwabi_hook_test_bind(text, text) RETURNS text AS '$bundle', 'kwabi_hook_test_bind' LANGUAGE C STRICT" >/dev/null 2>&1 || true
check "bind uint64 to v1" "bound" "$(Q "SELECT kwabi_hook_test_bind('uint64', '$v1')")"

check "v1 prints plain decimal" "42" "$(Q "SELECT '42'::uint64::text")"
check "max value round-trips" "18446744073709551615" "$(Q "SELECT '18446744073709551615'::uint64::text")"
Q "INSERT INTO tu VALUES (1, '1000'), (2, '18446744073709551615')" >/dev/null
check "stored values read back" "1000|18446744073709551615" "$(Q "SELECT string_agg(v::text, '|' ORDER BY id) FROM tu")"

check "bad syntax is 22P02" "22P02" "$(STATE "SELECT '4x'::uint64")"
check "control: a valid value is not an error" "ok" "$(STATE "SELECT '4'::uint64")"
check "overflow is 22003" "22003" "$(STATE "SELECT '18446744073709551616'::uint64")"
check "control: the same statement with a fitting value is not" "ok" "$(STATE "SELECT '18446744073709551615'::uint64")"

check "bind uint64 to v2 (reload)" "bound" "$(Q "SELECT kwabi_hook_test_bind('uint64', '$v2')")"
check "v2 prints with the prefix" "u42" "$(Q "SELECT '42'::uint64::text")"
check "stored bits are unchanged: the same rows, printed by v2" "u1000|u18446744073709551615" \
  "$(Q "SELECT string_agg(v::text, '|' ORDER BY id) FROM tu")"

check "bind refuses a missing path" "bind refused" "$(Q "SELECT kwabi_hook_test_bind('uint64', '$SCRATCH/missing.dylib')")"
check "after a refused bind, v2 still prints" "u42" "$(Q "SELECT '42'::uint64::text")"
check "a backend that starts after the refused bind still gets v2" "u42" "$(Q "SELECT '42'::uint64::text")"

# An unbound type name: the same C symbol, a name nothing bound.
"$bin/psql" -X -q -h 127.0.0.1 -p $port -U postgres -d postgres -v ON_ERROR_STOP=1 <<SQL >/dev/null
CREATE FUNCTION uint32_in(cstring) RETURNS uint64 AS '$bundle', 'kwabi_type_in' LANGUAGE C IMMUTABLE STRICT;
SQL
check "an unbound name is refused, not silently parsed" "XX000" \
  "$("$bin/psql" -X -q -At -h 127.0.0.1 -p $port -U postgres -d postgres -c "DO \$\$ BEGIN PERFORM uint32_in('1'); EXCEPTION WHEN others THEN RAISE NOTICE 'state=%', SQLSTATE; END \$\$;" 2>&1 | sed -n 's/.*state=//p')"

if [ "$fail" -eq 0 ]; then echo "uint64 canary passed (PG $major)"; else echo "FAILURES (PG $major)"; exit 1; fi
