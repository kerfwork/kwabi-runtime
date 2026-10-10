#!/usr/bin/env bash
# Bound aggregate bodies on a preloaded cluster, for one PostgreSQL major: builds the two
# body versions, runs aggregate-api.sql, and counts its checks.
#
#   SCRATCH=<dir> ./aggregate-api-run.sh <major>
#
# Build the bundle first (make build PG=<major>). Optional: PGBIN, DLSUFFIX, AGG_PORT.
set -euo pipefail
cd "$(dirname "$0")"
major="${1:?major}"
: "${SCRATCH:?set SCRATCH}"

bin="${PGBIN:-/opt/homebrew/opt/postgresql@$major/bin}"
dl="${DLSUFFIX:-dylib}"
bundle="$PWD/kwabi_runtime_pg$major.$dl"
port="${AGG_PORT:-$((5990 - major % 100))}"
data="$SCRATCH/aggregate-api-pg$major"
inc=$PWD/../vendor/kwabi/include
v1="$SCRATCH/agg_body_v1.$dl"
v2="$SCRATCH/agg_body_v2.$dl"
out="$SCRATCH/aggregate-api-$major.out"

cc -shared -fPIC -Wall -Wextra -I"$inc" -DSCALE=1 -o "$v1" aggregate-api/agg_body.c
cc -shared -fPIC -Wall -Wextra -I"$inc" -DSCALE=1000 -o "$v2" aggregate-api/agg_body.c

"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true
rm -rf "$data"
"$bin/initdb" -D "$data" -U postgres -A trust >"$SCRATCH/aggregate-api-initdb-$major.log" 2>&1
cat >> "$data/postgresql.conf" <<CONF
port = $port
listen_addresses = '127.0.0.1'
unix_socket_directories = ''
shared_preload_libraries = '$bundle'
CONF
"$bin/pg_ctl" -D "$data" -l "$SCRATCH/aggregate-api-server-$major.log" -w start >/dev/null
trap '"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true' EXIT

"$bin/psql" -X -q -At -h 127.0.0.1 -p "$port" -U postgres -d postgres \
  -v bundle="$bundle" -v v1="$v1" -v v2="$v2" -f aggregate-api.sql > "$out" 2>&1 || true

checks=$(grep -c "check .*: t$" "$out" || true)
fails=$(grep -c "check .*: f$" "$out" || true)
echo "PG$major: $checks passed, $fails failed"
grep "check .*: f$" "$out" || true
[ "$fails" -eq 0 ] && [ "$checks" -ge 16 ]
