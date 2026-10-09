#!/usr/bin/env bash
# Runs hook-api.sql against a scratch PostgreSQL cluster for one major.
#
#   SCRATCH=<dir> ./hook-api-run.sh <major>
#
# Builds nothing and installs nothing: build the bundle with `make build PG=<major>`
# first. The cluster is private (its own port and data directory), so the Homebrew
# server on the default port is never touched.
set -euo pipefail
cd "$(dirname "$0")"
major="${1:?major}"
: "${SCRATCH:?set SCRATCH to a directory for the scratch cluster}"

bin=/opt/homebrew/opt/postgresql@$major/bin
port=$((5700 + major))
data="$SCRATCH/hook-pg$major"
bundle="$PWD/kwabi_runtime_pg$major.dylib"

"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true
rm -rf "$data"
"$bin/initdb" -D "$data" -U postgres -A trust >"$SCRATCH/hook-initdb-$major.log" 2>&1
cat >> "$data/postgresql.conf" <<CONF
port = $port
listen_addresses = '127.0.0.1'
unix_socket_directories = ''
CONF
"$bin/pg_ctl" -D "$data" -l "$SCRATCH/hook-server-$major.log" -w start >/dev/null
trap '"$bin/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true' EXIT

"$bin/psql" -X -h 127.0.0.1 -p "$port" -U postgres -d postgres -v bundle="$bundle" \
  -f hook-api.sql 2>&1 | tee "$SCRATCH/hook-api-$major.out"

checks=$(grep -c "check .*: t$" "$SCRATCH/hook-api-$major.out" || true)
fails=$(grep -c "check .*: f$" "$SCRATCH/hook-api-$major.out" || true)
echo "PG$major: $checks passed, $fails failed"
[ "$fails" -eq 0 ] && [ "$checks" -gt 0 ]
