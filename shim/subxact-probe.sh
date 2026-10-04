#!/usr/bin/env bash
#
# subxact-probe.sh — prove fmgr-api check 13 can fail.
#
# check 13 asserts that a failed call through a call_function slot leaves no
# partial work. A check that has only ever passed is not evidence, so this
# script builds a deliberately-broken bundle with the subtransaction removed
# from shim_call_impl, runs the same probe against both, and requires:
#
#   shipped bundle  -> after_rows = 0   (the rollback worked)
#   stripped bundle -> after_rows = 1   (the write survived, so the check
#                                        discriminates rather than passing
#                                        for free)
#
# It is the fmgr counterpart of guard-control.sql and the survivors=0
# measurement in error-firewall-design.md §3.1.
#
# Usage: ./subxact-probe.sh [PG_MAJOR]   (default 18)

set -uo pipefail
cd "$(dirname "$0")"

PG="${1:-18}"
case "$PG" in
    16) PGC=/opt/homebrew/opt/postgresql@16/bin/pg_config; PORT=5434 ;;
    17) PGC=/opt/homebrew/opt/postgresql@17/bin/pg_config; PORT=5433 ;;
    18) PGC=pg_config;                                     PORT=5432 ;;
    *)  echo "PG must be 16, 17 or 18"; exit 2 ;;
esac

PGINC=$($PGC --includedir-server)
PKGLIB=$($PGC --pkglibdir)
PGBIN=$($PGC --bindir)
RUNTIME_DIR=..

VARIANT_SRC=/tmp/kwabi_shim_nosubxact.c
VARIANT_LIB=kwabi_nosubxact.dylib

# --- build the stripped variant -------------------------------------------
# Remove the three subtransaction calls from shim_call_impl only, via a helper
# script (a heredoc here would be fragile, and the replacement order matters).
python3 make_nosubxact_variant.py kwabi_runtime_shim.c "$VARIANT_SRC" \
  || { echo "FAIL: could not generate the stripped variant"; exit 1; }

cc -bundle -Wl,-undefined,dynamic_lookup -fPIC -w \
   -I"$PGINC" -I/opt/homebrew/include -I"$RUNTIME_DIR/.." -I"$RUNTIME_DIR" \
   -o "/tmp/$VARIANT_LIB" "$VARIANT_SRC" "$RUNTIME_DIR/target/release/libkwabi_runtime.a" \
  || { echo "FAIL: could not build the stripped variant"; exit 1; }

cp "/tmp/$VARIANT_LIB" "$PKGLIB/$VARIANT_LIB"

# --- run both -------------------------------------------------------------
run_probe() {  # run_probe <bundle> -> prints after_rows
    "$PGBIN/psql" -tA -p "$PORT" -d postgres -v ON_ERROR_STOP=0 \
        -v bundle="$1" -f subxact-probe.sql 2>/dev/null \
      | grep -E '^(0|1)$' | tail -1
}

SHIPPED=$(run_probe "kwabi_runtime_pg$PG.dylib")
STRIPPED=$(run_probe "$VARIANT_LIB")

rm -f "$PKGLIB/$VARIANT_LIB" "/tmp/$VARIANT_LIB" "$VARIANT_SRC"

echo "PG $PG  shipped=$SHIPPED  stripped=$STRIPPED  (expect 0 then 1)"
if [ "$SHIPPED" = "0" ] && [ "$STRIPPED" = "1" ]; then
    echo "OK: check 13 discriminates — it fails when the subtransaction is removed"
    exit 0
fi
echo "FAIL: the probe does not discriminate; check 13 is not evidence"
exit 1
