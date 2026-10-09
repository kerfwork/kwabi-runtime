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
fail=0

check() {
  if [ "$2" = "$3" ]; then echo "  PASS $1 ($3)"; else echo "  FAIL $1: want '$2' got '$3'"; fail=1; fi
}

# The body is the Rust uint core (canary-uint). The prefix is compile-time, so v1 and v2
# are two builds; cargo rebuilds the same target path, so each is copied out after building.
(cd canary-uint && KWABI_CANARY_PREFIX="" cargo build --release --quiet \
    && cp "target/release/libkwabi_uint_canary.$dl" "$v1" \
    && KWABI_CANARY_PREFIX="u" cargo build --release --quiet \
    && cp "target/release/libkwabi_uint_canary.$dl" "$v2")

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

# Operators: one SQL function per operator, all on the same symbol (kwabi_type_binop).
# The binding is the function's name minus its _<op> suffix.
ops_sql=""
for f in "eq:boolean" "ne:boolean" "lt:boolean" "le:boolean" "gt:boolean" "ge:boolean" \
         "add:uint64" "sub:uint64" "mul:uint64" "div:uint64" "mod:uint64" "cmp:int4"; do
  op=${f%%:*}; ret=${f##*:}
  ops_sql+="CREATE FUNCTION uint64_$op(uint64, uint64) RETURNS $ret AS '$bundle', 'kwabi_type_binop' LANGUAGE C IMMUTABLE STRICT;"$'\n'
done
"$bin/psql" -X -q -h 127.0.0.1 -p $port -U postgres -d postgres -v ON_ERROR_STOP=1 <<SQL
$ops_sql
CREATE OPERATOR = (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_eq, COMMUTATOR = =, NEGATOR = <>);
CREATE OPERATOR <> (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_ne, COMMUTATOR = <>, NEGATOR = =);
CREATE OPERATOR < (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_lt, COMMUTATOR = >, NEGATOR = >=);
CREATE OPERATOR <= (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_le, COMMUTATOR = >=, NEGATOR = >);
CREATE OPERATOR > (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_gt, COMMUTATOR = <, NEGATOR = <=);
CREATE OPERATOR >= (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_ge, COMMUTATOR = <=, NEGATOR = <);
CREATE OPERATOR + (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_add, COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_sub);
CREATE OPERATOR * (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_mul, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_div);
CREATE OPERATOR % (LEFTARG = uint64, RIGHTARG = uint64, FUNCTION = uint64_mod);
CREATE OPERATOR CLASS uint64_ops DEFAULT FOR TYPE uint64 USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
  FUNCTION 1 uint64_cmp(uint64, uint64);
SQL

check "eq true" "t" "$(Q "SELECT '5'::uint64 = '5'::uint64")"
check "eq control: unequal is false" "f" "$(Q "SELECT '5'::uint64 = '6'::uint64")"
check "ne true" "t" "$(Q "SELECT '4'::uint64 <> '5'::uint64")"
check "ne control: equal is false" "f" "$(Q "SELECT '4'::uint64 <> '4'::uint64")"
check "lt true" "t" "$(Q "SELECT '4'::uint64 < '5'::uint64")"
check "lt control: reversed is false" "f" "$(Q "SELECT '5'::uint64 < '4'::uint64")"
check "le true at equality" "t" "$(Q "SELECT '5'::uint64 <= '5'::uint64")"
check "le control: greater is false" "f" "$(Q "SELECT '6'::uint64 <= '5'::uint64")"
check "gt true" "t" "$(Q "SELECT '9'::uint64 > '5'::uint64")"
check "gt control: smaller is false" "f" "$(Q "SELECT '4'::uint64 > '5'::uint64")"
check "ge true at equality" "t" "$(Q "SELECT '4'::uint64 >= '4'::uint64")"
check "ge control: smaller is false" "f" "$(Q "SELECT '3'::uint64 >= '4'::uint64")"
check "max compares above 2^63" "t" "$(Q "SELECT '18446744073709551615'::uint64 > '9223372036854775807'::uint64")"

check "add" "5" "$(Q "SELECT ('2'::uint64 + '3'::uint64)::text")"
check "sub" "5" "$(Q "SELECT ('7'::uint64 - '2'::uint64)::text")"
check "mul" "12" "$(Q "SELECT ('3'::uint64 * '4'::uint64)::text")"
check "div truncates" "3" "$(Q "SELECT ('7'::uint64 / '2'::uint64)::text")"
check "mod" "3" "$(Q "SELECT ('7'::uint64 % '4'::uint64)::text")"
check "add overflow is 22003" "22003" "$(STATE "SELECT '18446744073709551615'::uint64 + '1'::uint64")"
check "control: add at the edge is fine" "ok" "$(STATE "SELECT '18446744073709551614'::uint64 + '1'::uint64")"
check "sub underflow is 22003" "22003" "$(STATE "SELECT '1'::uint64 - '2'::uint64")"
check "mul overflow is 22003" "22003" "$(STATE "SELECT '4294967296'::uint64 * '4294967296'::uint64")"
check "division by zero is 22012" "22012" "$(STATE "SELECT '7'::uint64 / '0'::uint64")"
check "modulo by zero is 22012" "22012" "$(STATE "SELECT '7'::uint64 % '0'::uint64")"
check "control: division by one is fine" "ok" "$(STATE "SELECT '7'::uint64 / '1'::uint64")"

# Ordering through the btree opclass: the index is only usable if cmp is.
Q "CREATE TABLE tops (v uint64)" >/dev/null
Q "CREATE INDEX tops_v ON tops (v)" >/dev/null
Q "INSERT INTO tops VALUES ('18446744073709551615'), ('1000'), ('7')" >/dev/null
check "ORDER BY uses cmp: sorted" "7,1000,18446744073709551615" \
  "$(Q "SELECT string_agg(v::text, ',' ORDER BY v) FROM tops")"
check "a btree index serves equality" "Index" \
  "$(Q "SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT v FROM tops WHERE v = '1000'" | grep -o 'Index' | head -1)"
check "control: the index returns the right row" "1000" \
  "$(Q "SET enable_seqscan = off; SELECT v::text FROM tops WHERE v = '1000'")"

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
