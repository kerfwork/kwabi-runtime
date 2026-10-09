#!/usr/bin/env bash
#
# ci-local.sh — the local, single-architecture CI matrix.
#
# This is the smallest thing that catches the class of bug the project exists
# to eliminate: an ABI change that works on one PostgreSQL major and breaks on
# another. It does that with two assertions, in this order:
#
#   1. BUILD-ONCE  — the canary is compiled once and its hash is pinned. It is
#                    installed unchanged into every major's $libdir. If a major
#                    needs its own copy of the extension, the matrix fails.
#   2. MATRIX      — each major gets its own runtime bundle, and the same
#                    pinned canary must pass all 13 checks in each server.
#
# It is deliberately local and single-architecture (arm64). Cross-architecture
# (x86_64) is a separate exercise: it needs a second toolchain and a second
# Postgres build, and it tests a different failure mode (struct layout and
# calling convention), not ABI drift across majors.
#
# Usage:
#   ./ci-local.sh              # all configured majors
#   ./ci-local.sh 17 18        # a subset
#   ./ci-local.sh --no-servers # build/compile-check only, no running servers
#
set -uo pipefail

cd "$(dirname "$0")"

# Platform-specific dynamic library suffix
case "$(uname -s)" in
    Darwin) DLSUFFIX=dylib ;;
    *)      DLSUFFIX=so ;;
esac

# Portable SHA-256 helper
SHA256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

MAJORS_ALL=(16 17 18)

# ---------------------------------------------------------------------------
# Matrix definition
# ---------------------------------------------------------------------------

pg_config_for() {
    local major="$1"
    # (a) env override: PG_CONFIG_pg16, PG_CONFIG_pg17, PG_config_pg18
    local var="PG_CONFIG_pg$major"
    if [ -n "${!var:-}" ]; then
        echo "${!var}"
        return
    fi
    # (b) command -v pg_config$major
    if command -v "pg_config$major" >/dev/null 2>&1; then
        echo "pg_config$major"
        return
    fi
    # (c) macOS default
    if [ -x "/opt/homebrew/opt/postgresql@$major/bin/pg_config" ]; then
        echo "/opt/homebrew/opt/postgresql@$major/bin/pg_config"
        return
    fi
    # (d) Linux default
    if [ -x "/usr/lib/postgresql/$major/bin/pg_config" ]; then
        echo "/usr/lib/postgresql/$major/bin/pg_config"
        return
    fi
    return 1
}

pg_bin_for() {
    local pgc
    pgc=$(pg_config_for "$1") || return 1
    "$pgc" --bindir
}

port_for() {
    case "$1" in
        16) echo 5434 ;;
        17) echo 5433 ;;
        18) echo 5432 ;;
    esac
}

data_for() {
    echo "$(cd .. && pwd)/pg$1/data"
}

# SLRU runs against its OWN throwaway cluster, not the matrix server on :$PORT.
# It needs the runtime in shared_preload_libraries and an SLRU declared in
# kwabi.slrus, and neither can be changed on a running server (both are
# PGC_POSTMASTER). Restarting the matrix server with preload would change the
# environment every other check runs in — and a preloaded runtime is exactly the
# condition under which the SLRU capability bit flips on. So the stage starts a
# private cluster, runs, and tears it down.
slru_port_for() {
    case "$1" in
        16) echo 5464 ;;
        17) echo 5463 ;;
        18) echo 5462 ;;
    esac
}

slru_data_for() {
    echo "${TMPDIR:-/tmp}/kwabi-slru-pg$1"
}

# The unix-socket directory differs by platform: macOS PostgreSQL defaults to
# /tmp, Debian/Ubuntu to /var/run/postgresql. Probing for the directory is not
# enough -- Debian creates /var/run/postgresql even when the server is listening
# elsewhere -- so each candidate is probed for an actual socket file, and a
# server already running on any candidate is found. An explicit PGHOST wins.
PSOCK="${PGHOST:-}"
if [ -z "$PSOCK" ]; then
    for _d in /var/run/postgresql /tmp; do
        if ls "$_d"/.s.PGSQL.* >/dev/null 2>&1; then
            PSOCK="$_d"
            break
        fi
    done
    # No live socket anywhere: fall back to the platform default so the
    # start-a-server path below has somewhere to look.
    [ -z "$PSOCK" ] && { [ -d /var/run/postgresql ] && PSOCK=/var/run/postgresql || PSOCK=/tmp; }
fi
echo "socket dir   : $PSOCK"

# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

PASS=0
FAIL=0
SKIP=0
declare -a RESULTS

record() {  # record <status> <major> <what>
    RESULTS+=("$(printf '%-4s %-4s %s' "$1" "$2" "$3")")
    case "$1" in
        PASS) PASS=$((PASS+1)) ;;
        FAIL) FAIL=$((FAIL+1)) ;;
        *)    SKIP=$((SKIP+1)) ;;
    esac
}

hr() { printf '%s\n' "------------------------------------------------------------"; }

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------

RUN_SERVERS=1
ARGS=()
for a in "$@"; do
    case "$a" in
        --no-servers) RUN_SERVERS=0 ;;
        *) ARGS+=("$a") ;;
    esac
done
MAJORS=("${MAJORS_ALL[@]}")
[ ${#ARGS[@]} -gt 0 ] && MAJORS=("${ARGS[@]}")

echo "kwabi local CI matrix"
hr
echo "architecture : $(uname -m) ($(uname -s))"
echo "majors       : ${MAJORS[*]}"
echo "servers      : $([ $RUN_SERVERS -eq 1 ] && echo 'yes' || echo 'no (build only)')"
hr

# ---------------------------------------------------------------------------
# Step 0: the Rust core
# ---------------------------------------------------------------------------

echo
echo "[core] cargo test"
# Note: not `cargo test | grep -q`. grep -q exits as soon as it matches, which
# sends SIGPIPE to cargo; under `set -o pipefail` that fails the whole pipeline
# even though the tests passed. Capture the output, then search it.
CARGO_OUT=$(cd .. && cargo test 2>&1)
CARGO_RC=$?
if [ $CARGO_RC -eq 0 ] && printf '%s' "$CARGO_OUT" | grep -q 'test result: ok'; then
    record PASS core "cargo test"
else
    record FAIL core "cargo test (rc=$CARGO_RC)"
    printf '%s\n' "$CARGO_OUT" | grep -E 'error|FAILED' | head -5 | sed 's/^/      /'
fi

# Locate the kwabi header, relative to the CRATE root (one level up from shim/).
#
# Two layouts exist and they must not be confused. The published repo vendors
# the SDK crate and tracks exactly one header, vendor/kwabi/kwabi.h. The private
# source tree additionally carries a stale copy at the crate root, which is NOT
# the source of truth -- abi.rs is generated from the vendored one. So the
# vendored paths are tried FIRST and the crate-root copy is only a fallback, or
# a stale private copy would silently be treated as authoritative.
KWABI_HDR=""
for cand in vendor/kwabi/kwabi.h vendor/kwabi/include/kwabi.h kwabi.h; do
    if [ -f "../$cand" ]; then KWABI_HDR="$cand"; break; fi
done
if [ -z "$KWABI_HDR" ]; then
    record FAIL core "kwabi.h not found (looked in vendor/kwabi/ and the crate root)"
fi

echo "[core] src/abi.rs matches kwabi.h"
if [ -n "$KWABI_HDR" ] && (cd .. && python3 gen_kwabi_struct.py "$KWABI_HDR" | rustfmt --edition 2021 > /tmp/kwabi_abi_check.rs \
      && diff -q /tmp/kwabi_abi_check.rs src/abi.rs >/dev/null); then
    record PASS core "src/abi.rs is current"
else
    record FAIL core "src/abi.rs is stale — run gen_kwabi_struct.py"
fi

echo "[core] header compiles standalone"
if [ -n "$KWABI_HDR" ] && cc -fsyntax-only -Wall -Wextra -x c "$(cd .. && pwd)/$KWABI_HDR" 2>/dev/null; then
    record PASS core "kwabi.h compiles"
else
    record FAIL core "kwabi.h does not compile"
fi

# ---------------------------------------------------------------------------
# Step 1: BUILD-ONCE — compile the canary a single time, pin its hash
# ---------------------------------------------------------------------------

echo
echo "[canary] build once"
make -s canary >/dev/null 2>&1
CANARY="../canary/libcanary.$DLSUFFIX"

if [ ! -f "$CANARY" ]; then
    echo "FATAL: canary was not built"
    exit 1
fi

CANARY_HASH=$(SHA256 "$CANARY" | cut -d' ' -f1)
echo "  path : $CANARY"
echo "  hash : $CANARY_HASH"

# The guarded-body canary is a second version-independent artifact. It must be
# pinned the same way, or a major could quietly get its own copy.
GUARDED_CANARY="../canary/guarded/target/release/libguarded_canary.$DLSUFFIX"
make -s guard-build >/dev/null 2>&1
if [ ! -f "$GUARDED_CANARY" ]; then
    record FAIL canary "guarded canary was not built"
    exit 1
fi
GUARDED_HASH=$(SHA256 "$GUARDED_CANARY" | cut -d' ' -f1)
echo "  guarded: $GUARDED_HASH"

# The claim is "links no PostgreSQL symbol", so that is what is asserted -- not
# "zero undefined symbols". A macOS -shared object genuinely has none, which is
# why the stricter form used to pass; a Linux shared object always carries a few
# weak libc/init symbols (__cxa_finalize, __gmon_start__, the ITM clone table).
BAD_SYMS=$(nm -u "$CANARY" 2>/dev/null \
           | grep -iE 'palloc|pfree|elog|ereport|SPI_|heap_|_PG_|Relation|TupleDesc|MemoryContext' || true)
if [ -z "$BAD_SYMS" ]; then
    record PASS canary "references no PostgreSQL symbol"
else
    record FAIL canary "references PostgreSQL symbols: $BAD_SYMS"
fi

# ---------------------------------------------------------------------------
# Step 2: MATRIX
# ---------------------------------------------------------------------------

for M in "${MAJORS[@]}"; do
    echo
    hr
    echo "PostgreSQL $M"
    hr

    PGC=$(pg_config_for "$M")
    PGB=$(pg_bin_for "$M")
    PORT=$(port_for "$M")

    # `command -v` handles bare names like `pg_config`; a `-x` test does not.
    if ! command -v "$PGC" >/dev/null 2>&1; then
        # A DECLARED major that cannot be resolved is a FAIL, not a SKIP.
        # Only undeclared majors (not in MAJORS_ALL) may SKIP.
        declared=0
        for dm in "${MAJORS_ALL[@]}"; do
            [ "$dm" = "$M" ] && declared=1 && break
        done
        if [ $declared -eq 1 ]; then
            echo "  FAIL: no pg_config for declared major $M"
            record FAIL "$M" "pg_config not found (declared major)"
        else
            echo "  skip: no pg_config for $M (not a declared major)"
            record SKIP "$M" "not installed"
        fi
        continue
    fi

    # --- the shim must compile against this major's headers -------------
    echo "  [build] shim"
    if make -s build PG="$M" >/tmp/kwabi_build_$M.log 2>&1; then
        record PASS "$M" "shim compiles"
    else
        record FAIL "$M" "shim failed to compile"
        echo "      see /tmp/kwabi_build_$M.log"
        grep -E 'error|Error' /tmp/kwabi_build_$M.log | head -5 | sed 's/^/      /'
        continue
    fi

    # --- the pinned canary must not have been rebuilt -------------------
    NOW=$(SHA256 "$CANARY" | cut -d' ' -f1)
    if [ "$NOW" != "$CANARY_HASH" ]; then
        record FAIL "$M" "canary was rebuilt — build-once violated"
        continue
    fi
    GNOW=$(SHA256 "$GUARDED_CANARY" | cut -d' ' -f1)
    if [ "$GNOW" != "$GUARDED_HASH" ]; then
        record FAIL "$M" "guarded canary was rebuilt — build-once violated"
        continue
    fi
    record PASS "$M" "canaries unchanged"

    # --- install both objects -------------------------------------------
    PKGLIB=$("$PGC" --pkglibdir)
    if ! make -s install PG="$M" >/dev/null 2>&1; then
        record FAIL "$M" "install into $PKGLIB failed"
        continue
    fi

    # The guarded-body canary is a separate crate (it depends on the Rust SDK),
    # so it is not part of `install`. It is version-independent like the plain
    # canary: built once, installed into every $libdir.
    if ! make -s guard-build PG="$M" >/dev/null 2>&1; then
        record FAIL "$M" "guarded canary failed to build"
        echo "      see: cd ../canary/guarded && cargo build --release"
        continue
    fi
    if [ ! -f "$PKGLIB/libguarded_canary.$DLSUFFIX" ]; then
        record FAIL "$M" "guarded canary not installed into $PKGLIB"
        continue
    fi

    # The cross-version error test is its own module and does not use the
    # runtime at all — it exercises the size protocol directly.
    if ! make -s -C ../errsize install PG="$M" >/dev/null 2>&1; then
        record FAIL "$M" "errsize module failed to build/install"
        continue
    fi

    if [ $RUN_SERVERS -eq 0 ]; then
        record SKIP "$M" "server checks (--no-servers)"
        continue
    fi

    # --- is a server listening? -----------------------------------------
    if ! "$PGB/pg_isready" -h "$PSOCK" -p "$PORT" -q 2>/dev/null; then
        echo "  no server on :$PORT — attempting to start"
        DATA=$(data_for "$M")
        if [ -f "$DATA/PG_VERSION" ]; then
            LC_ALL="en_US.UTF-8" LANG="en_US.UTF-8" \
                "$PGB/pg_ctl" -D "$DATA" -l "$DATA/server.log" start >/dev/null 2>&1
            sleep 2
        fi
    fi

    if ! "$PGB/pg_isready" -h "$PSOCK" -p "$PORT" -q 2>/dev/null; then
        record SKIP "$M" "no server on :$PORT"
        continue
    fi

    # --- the 13 checks ---------------------------------------------------
    echo "  [proof] 13 checks against :$PORT"
    OUT=/tmp/kwabi_proof_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -v canary="libcanary.$DLSUFFIX" \
        -v libdir="$PKGLIB" \
        -f proof.sql postgres >"$OUT" 2>&1

    ABI_LINE=$(grep -c "kwabi ABI v1" "$OUT")
    GENUINE=$(grep -c "genuine PostgreSQL memory" "$OUT")
    FOREIGN=$(grep -c "foreign extension reached" "$OUT")
    LOADED=$(grep -c "extension loaded through the ABI" "$OUT")
    SQLSTATE=$(grep -c "SQLSTATE 22012" "$OUT")
    ERRORS=$(grep -c "ERROR:" "$OUT")

    # Exactly one deliberate ERROR is expected (check 7). More means something
    # else failed; none means the deliberate raise did not happen.
    if [ "$ABI_LINE" -ge 1 ] && [ "$GENUINE" -ge 2 ] && \
       [ "$FOREIGN" -ge 2 ] && [ "$LOADED" -ge 1 ] && \
       [ "$SQLSTATE" -ge 1 ] && [ "$ERRORS" -eq 1 ]; then
        record PASS "$M" "13 proof checks green"
    else
        record FAIL "$M" "proof: abi=$ABI_LINE mem=$GENUINE foreign=$FOREIGN loaded=$LOADED sqlstate=$SQLSTATE errs=$ERRORS"
        echo "      see $OUT"
    fi

    # --- error firewall: kwabi_try --------------------------------------
    #
    # The subtlest code in the project, so it gets regression protection like
    # everything else. The assertion that matters is `survivors=0`: a body that
    # fails must leave no work behind. A firewall that merely does not crash
    # would pass every other check here.
    echo "  [try] error firewall against :$PORT"
    OUT=/tmp/kwabi_try_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -v canary="libcanary.$DLSUFFIX" \
        -v libdir="$PKGLIB" \
        -f try.sql postgres >"$OUT" 2>&1

    TRY_UNDONE=$(grep -c "correct: work undone" "$OUT")
    TRY_OK=$(grep -c "status=0 body_ran=1" "$OUT")
    TRY_NESTED=$(grep -c "outer_status=0 inner_survivors=0" "$OUT")
    TRY_RAISED=$(grep -c "status=1 sqlstate=[0-9]" "$OUT")
    TRY_LEAK=$(grep -c "WRONG: partial work survived" "$OUT")

    # 4 undone-work results: check 3, 7, 8's 20-try loop, and check 9.
    if [ "$TRY_UNDONE" -ge 3 ] && [ "$TRY_OK" -ge 1 ] && \
       [ "$TRY_NESTED" -ge 1 ] && [ "$TRY_RAISED" -ge 1 ] && [ "$TRY_LEAK" -eq 0 ]; then
        record PASS "$M" "error firewall green"
    else
        record FAIL "$M" "try: undone=$TRY_UNDONE ok=$TRY_OK nested=$TRY_NESTED raised=$TRY_RAISED leaked=$TRY_LEAK"
        echo "      see $OUT"
    fi

    # --- guarded bodies: panic containment ------------------------------
    #
    # The control (an unguarded body aborting) runs separately, because it
    # takes the postmaster down. See guard-control.sql.
    echo "  [guard] panic containment against :$PORT"
    OUT=/tmp/kwabi_guard_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -v libdir="$PKGLIB" \
        -v guarded="libguarded_canary.$DLSUFFIX" \
        -f guard-test.sql postgres >"$OUT" 2>&1

    GUARD_PANIC=$(grep -c "status=2 " "$OUT")
    GUARD_ALIVE=$(grep -c "^ alive$" "$OUT")
    GUARD_TXN=$(grep -c "^ *42$" "$OUT")
    GUARD_CRASH=$(grep -c "connection to server was lost" "$OUT")

    # status=2 twice (check 3 and check 5), the backend alive after each, and
    # the transaction usable. A crash here means the macro did not contain.
    if [ "$GUARD_PANIC" -ge 2 ] && [ "$GUARD_ALIVE" -ge 1 ] && \
       [ "$GUARD_TXN" -ge 1 ] && [ "$GUARD_CRASH" -eq 0 ]; then
        record PASS "$M" "panic containment green"
    else
        record FAIL "$M" "guard: panicked=$GUARD_PANIC alive=$GUARD_ALIVE txn=$GUARD_TXN crash=$GUARD_CRASH"
        echo "      see $OUT"
    fi

    # --- capabilities: the bitset must be HONEST -------------------------
    #
    # A capability bit that lies is worse than no bit, because an extension
    # relies on it. So the check requires both directions: every claimed bit
    # proven behaviourally, AND no undefined bit set. A runtime returning
    # all-ones passes the first and fails the second.
    echo "  [capabilities] bitset honesty against :$PORT"
    OUT=/tmp/kwabi_caps_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=1 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -v canary="$PKGLIB/libguarded_canary.$DLSUFFIX" \
        -f capabilities.sql postgres >"$OUT" 2>&1

    # All seven assertions true.
    CAP_TRUE=$(grep -cE '^ t *$' "$OUT")
    CAP_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The named ones, so a failure says which.
    CAP_CORE=$(grep -A2 'core_true' "$OUT" | grep -cE '^ t')
    CAP_ATOMIC=$(grep -A2 'atomic_body_true' "$OUT" | grep -cE '^ t')
    CAP_UNDEF=$(grep -A2 'no_undefined_bits' "$OUT" | grep -cE '^ t')

    if [ "$CAP_FALSE" -eq 0 ] && [ "$CAP_CORE" -ge 1 ] && \
       [ "$CAP_ATOMIC" -ge 1 ] && [ "$CAP_UNDEF" -ge 1 ]; then
        record PASS "$M" "capability bitset honest ($CAP_TRUE assertions)"
    else
        record FAIL "$M" "capabilities: true=$CAP_TRUE false=$CAP_FALSE core=$CAP_CORE atomic=$CAP_ATOMIC undef=$CAP_UNDEF"
        echo "      see $OUT"
    fi

    # --- type-api: type system through the ABI ---------------------------
    echo "  [type-api] type system against :$PORT"
    OUT=/tmp/kwabi_type_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -v canary="libcanary.$DLSUFFIX" \
        -v libdir="$PKGLIB" \
        -f type-api.sql postgres >"$OUT" 2>&1

    TYPE_TRUE=$(grep -cE '^ t *$' "$OUT")
    TYPE_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The named ones, so a failure says which.
    TYPE_LEN=$(grep -A2 'type_length_int4' "$OUT" | grep -cE '^ t')
    TYPE_ARR=$(grep -A2 'type_is_array_int4_array' "$OUT" | grep -cE '^ t')
    TYPE_IN=$(grep -A2 'type_input_int4' "$OUT" | grep -cE '^ t')
    TYPE_OUT=$(grep -A2 'type_output_int4' "$OUT" | grep -cE '^ t')
    TYPE_SEND=$(grep -A2 'type_send_int4' "$OUT" | grep -cE '^ t')
    TYPE_RECV=$(grep -A2 'type_recv_int4' "$OUT" | grep -cE '^ t')

    if [ "$TYPE_FALSE" -eq 0 ] && [ "$TYPE_LEN" -ge 1 ] && \
       [ "$TYPE_ARR" -ge 1 ] && [ "$TYPE_IN" -ge 1 ] && [ "$TYPE_OUT" -ge 1 ] && \
       [ "$TYPE_SEND" -ge 1 ] && [ "$TYPE_RECV" -ge 1 ]; then
        record PASS "$M" "type-api green ($TYPE_TRUE assertions)"
    else
        record FAIL "$M" "type-api: true=$TYPE_TRUE false=$TYPE_FALSE len=$TYPE_LEN arr=$TYPE_ARR in=$TYPE_IN out=$TYPE_OUT send=$TYPE_SEND recv=$TYPE_RECV"
        echo "      see $OUT"
    fi

    # --- capabilities: CONSUMED by an extension, not just read by the shim ---
    #
    # The section above proves the bitset is honest. This one proves an
    # extension can act on it: a body that asks for a guarantee it needs and
    # refuses when the runtime does not offer it.
    #
    # The assertion that matters is `paths_agree`. The shim and the extension
    # reach the bitset by different routes -- the shim reads the published
    # table; the extension goes through kwabi_ext_init's stored pointer and the
    # SDK's mirror of the struct. If the mirror drifts from kwabi.h (it was six
    # slots short before this test existed), or the bootstrap rule is applied as
    # zero instead of CORE-only, this disagrees while every shim-side check
    # above stays green.
    echo "  [capabilities] consumed by an extension against :$PORT"
    OUT=/tmp/kwabi_capconsume_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=1 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -v libdir="$PKGLIB" \
        -v guarded="libguarded_canary.$DLSUFFIX" \
        -f capability-consumer.sql postgres >"$OUT" 2>&1

    CC_TRUE=$(grep -cE '^ t *$' "$OUT")
    CC_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The three that carry the meaning, so a failure says which.
    CC_REACHED=$(grep -A2 'body_reached_the_runtime' "$OUT" | grep -cE '^ t')
    CC_AGREE=$(grep -A2 'paths_agree' "$OUT" | grep -cE '^ t')
    CC_REFUSED=$(grep -A2 'refused_when_unguaranteed' "$OUT" | grep -cE '^ t')

    if [ "$CC_FALSE" -eq 0 ] && [ "$CC_REACHED" -ge 1 ] && \
       [ "$CC_AGREE" -ge 1 ] && [ "$CC_REFUSED" -ge 1 ]; then
        record PASS "$M" "capabilities consumed by an extension ($CC_TRUE assertions)"
    else
        record FAIL "$M" "cap-consumer: true=$CC_TRUE false=$CC_FALSE reached=$CC_REACHED agree=$CC_AGREE refused=$CC_REFUSED"
        echo "      see $OUT"
    fi

    # --- mem-api: memory context lifecycle -------------------------------
    #
    # The full lifecycle: create -> switch -> alloc -> verify -> name -> reset
    # -> delete. Every check is a boolean, so the harness counts `t` and `f`
    # rather than grepping for a success message -- message greps are sensitive
    # to wording and to how many times a line is printed, which is exactly how
    # the first version of this check passed while printing nothing.
    #
    # ON_ERROR_STOP is off: the negative control (check 4) raises by design.
    echo "  [mem-api] memory context lifecycle against :$PORT"
    OUT=/tmp/kwabi_memapi_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f mem-api.sql postgres >"$OUT" 2>&1

    MEM_TRUE=$(grep -cE '^ t *$' "$OUT")
    MEM_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired; its
    # absence means the wrong-name comparison matched and the test is vacuous.
    MEM_CONTROL=$(grep -c "negative control fired as intended" "$OUT")
    MEM_LIFECYCLE=$(grep -A2 'lifecycle_verified' "$OUT" | grep -cE '^ t')
    MEM_IN_TXN=$(grep -A2 'in_transaction' "$OUT" | grep -cE '^ t')
    MEM_AFTER=$(grep -A2 'after_control' "$OUT" | grep -cE '^ t')

    if [ "$MEM_FALSE" -eq 0 ] && [ "$MEM_CONTROL" -ge 1 ] && \
       [ "$MEM_LIFECYCLE" -ge 1 ] && [ "$MEM_IN_TXN" -ge 1 ] && \
       [ "$MEM_AFTER" -ge 1 ]; then
        record PASS "$M" "mem-api lifecycle green ($MEM_TRUE assertions)"
    else
        record FAIL "$M" "mem-api: true=$MEM_TRUE false=$MEM_FALSE control=$MEM_CONTROL lifecycle=$MEM_LIFECYCLE in_txn=$MEM_IN_TXN after=$MEM_AFTER"
        echo "      see $OUT"
    fi

    # --- fmgr-api: function lookup and calls -----------------------------
    #
    # The function-call group, the base every other ABI group builds on. Two
    # things here are the reason this file exists rather than reusing try.sql:
    #
    #   * the NULL result. The slots used to be declared with a bare `Datum`
    #     return, which has no null channel -- a NULL result would have been
    #     indistinguishable from 0. The test calls a function that returns NULL
    #     and requires it to be reported AS NULL.
    #   * the variadic `call_function` slot. PostgreSQL has no N-ary call
    #     helper, so the shim builds the FunctionCallInfo by hand; that path
    #     needs its own assertion, not an assumption that the fixed-arity
    #     slots cover it.
    #
    # ON_ERROR_STOP is off: check 8 (the negative control) raises by design.
    echo "  [fmgr-api] function calls against :$PORT"
    OUT=/tmp/kwabi_fmgr_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f fmgr-api.sql postgres >"$OUT" 2>&1

    FMGR_TRUE=$(grep -cE '^ t *$' "$OUT")
    FMGR_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    FMGR_CONTROL=$(grep -c "fmgr negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    FMGR_ONENULL=$(grep -A2 'null_result_reported' "$OUT" | grep -cE '^ t')
    FMGR_ERRCODE=$(grep -A2 'error_caught_with_sqlstate' "$OUT" | grep -cE '^ t')
    FMGR_ONEARG=$(grep -A2 'one_arg_call' "$OUT" | grep -cE '^ t')
    # THE load-bearing one: a failed call must leave no partial work. This is
    # the check that distinguishes a firewall that works from one that merely
    # does not crash; without it the subtransaction could be removed and every
    # other assertion here would still pass.
    FMGR_NOSURVIVOR=$(grep -A2 'no_partial_work_survived' "$OUT" | grep -cE '^ t')

    if [ "$FMGR_FALSE" -eq 0 ] && [ "$FMGR_CONTROL" -ge 1 ] && \
       [ "$FMGR_ONENULL" -ge 1 ] && [ "$FMGR_ERRCODE" -ge 1 ] && \
       [ "$FMGR_ONEARG" -ge 1 ] && [ "$FMGR_NOSURVIVOR" -ge 1 ]; then
        record PASS "$M" "fmgr-api green ($FMGR_TRUE assertions)"
    else
        record FAIL "$M" "fmgr-api: true=$FMGR_TRUE false=$FMGR_FALSE control=$FMGR_CONTROL null_result=$FMGR_ONENULL sqlstate=$FMGR_ERRCODE one_arg=$FMGR_ONEARG no_partial_work=$FMGR_NOSURVIVOR"
        echo "      see $OUT"
    fi

    # --- guc-api: GUC access through the ABI -----------------------------
    #
    # The GUC group: read and write configuration values. The assertion that
    # matters is `set_and_read_back` -- it proves that a write through the ABI
    # is committed and readable, not just that the ABI can call
    # GetConfigOptionByName.
    #
    # ON_ERROR_STOP is off: check 8 (the negative control) raises by design.
    echo "  [guc-api] GUC access against :$PORT"
    OUT=/tmp/kwabi_guc_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f guc-api.sql postgres >"$OUT" 2>&1

    GUC_TRUE=$(grep -cE '^ t *$' "$OUT")
    GUC_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    GUC_CONTROL=$(grep -c "GUC negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    GUC_INT=$(grep -A2 'int_guc_read' "$OUT" | grep -cE '^ t')
    GUC_SET=$(grep -A2 'set_and_read_back' "$OUT" | grep -cE '^ t')
    GUC_BOOL=$(grep -A2 'bool_guc_read' "$OUT" | grep -cE '^ t')

    if [ "$GUC_FALSE" -eq 0 ] && [ "$GUC_CONTROL" -ge 1 ] && \
       [ "$GUC_INT" -ge 1 ] && [ "$GUC_SET" -ge 1 ] && \
       [ "$GUC_BOOL" -ge 1 ]; then
        record PASS "$M" "guc-api green ($GUC_TRUE assertions)"
    else
        record FAIL "$M" "guc-api: true=$GUC_TRUE false=$GUC_FALSE control=$GUC_CONTROL int=$GUC_INT set=$GUC_SET bool=$GUC_BOOL"
        echo "      see $OUT"
    fi

    # --- defrem-api: column default operations through the ABI ------------
    #
    # The defrem group: create, alter, and drop column defaults. The
    # assertion that matters is `defrem_lifecycle` -- it proves that a
    # default set through the ABI is committed and readable.
    #
    # ON_ERROR_STOP is off: check 5 (the negative control) raises by design.
    echo "  [defrem-api] column defaults against :$PORT"
    OUT=/tmp/kwabi_defrem_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f defrem-api.sql postgres >"$OUT" 2>&1

    DEFREM_TRUE=$(grep -cE '^ t *$' "$OUT")
    DEFREM_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    DEFREM_CONTROL=$(grep -c "defrem negative control fired as intended" "$OUT")
    # The two that carry the meaning, so a failure says which.
    DEFREM_LIFECYCLE=$(grep -A2 'defrem_lifecycle' "$OUT" | grep -cE '^ t')
    DEFREM_ALIVE=$(grep -A2 'still_alive' "$OUT" | grep -cE '^ t')

    if [ "$DEFREM_FALSE" -eq 0 ] && [ "$DEFREM_CONTROL" -ge 1 ] && \
       [ "$DEFREM_LIFECYCLE" -ge 1 ] && [ "$DEFREM_ALIVE" -ge 1 ]; then
        record PASS "$M" "defrem-api green ($DEFREM_TRUE assertions)"
    else
        record FAIL "$M" "defrem-api: true=$DEFREM_TRUE false=$DEFREM_FALSE control=$DEFREM_CONTROL lifecycle=$DEFREM_LIFECYCLE alive=$DEFREM_ALIVE"
        echo "      see $OUT"
    fi

    # --- spi-api: SQL execution through the ABI --------------------------
    #
    # The SPI group: execute queries, read results. The assertion that
    # matters is `insert_then_select` -- it proves that a write through the
    # ABI is committed and readable, not just that the ABI can call
    # SPI_execute.
    #
    # ON_ERROR_STOP is off: check 5 (the negative control) raises by design.
    echo "  [spi-api] SQL execution against :$PORT"
    OUT=/tmp/kwabi_spi_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f spi-api.sql postgres >"$OUT" 2>&1

    SPI_TRUE=$(grep -cE '^ t *$' "$OUT")
    SPI_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    SPI_CONTROL=$(grep -c "SPI negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    SPI_SELECT=$(grep -A2 'select_works' "$OUT" | grep -cE '^ t')
    SPI_INSERT=$(grep -A2 'insert_then_select' "$OUT" | grep -cE '^ t')
    SPI_WRITE=$(grep -A2 'write_visible' "$OUT" | grep -cE '^ t')

    if [ "$SPI_FALSE" -eq 0 ] && [ "$SPI_CONTROL" -ge 1 ] && \
       [ "$SPI_SELECT" -ge 1 ] && [ "$SPI_INSERT" -ge 1 ] && \
       [ "$SPI_WRITE" -ge 1 ]; then
        record PASS "$M" "spi-api green ($SPI_TRUE assertions)"
    else
        record FAIL "$M" "spi-api: true=$SPI_TRUE false=$SPI_FALSE control=$SPI_CONTROL select=$SPI_SELECT insert=$SPI_INSERT write=$SPI_WRITE"
        echo "      see $OUT"
    fi

    # --- parser-api: parser slots through the ABI -------------------------
    #
    # The parser group: parse expressions, parse type names, query operator
    # properties. The assertion that matters is `parse_expr_works` -- it
    # proves that parse_expr, node_type_name, and free_node all work together.
    #
    # ON_ERROR_STOP is off: check 10 (the negative control) raises by design.
    echo "  [parser-api] parser slots against :$PORT"
    OUT=/tmp/kwabi_parser_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f parser-api.sql postgres >"$OUT" 2>&1

    PARSER_TRUE=$(grep -cE '^ t *$' "$OUT")
    PARSER_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    PARSER_CONTROL=$(grep -c "parser negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    PARSER_EXPR=$(grep -A2 'parse_expr_works' "$OUT" | grep -cE '^ t')
    PARSER_TYPE=$(grep -A2 'parse_type_works' "$OUT" | grep -cE '^ t')
    PARSER_OPER=$(grep -A2 'oper_left_type_int4' "$OUT" | grep -cE '^ t')

    if [ "$PARSER_FALSE" -eq 0 ] && [ "$PARSER_CONTROL" -ge 1 ] && \
       [ "$PARSER_EXPR" -ge 1 ] && [ "$PARSER_TYPE" -ge 1 ] && \
       [ "$PARSER_OPER" -ge 1 ]; then
        record PASS "$M" "parser-api green ($PARSER_TRUE assertions)"
    else
        record FAIL "$M" "parser-api: true=$PARSER_TRUE false=$PARSER_FALSE control=$PARSER_CONTROL expr=$PARSER_EXPR type=$PARSER_TYPE oper=$PARSER_OPER"
        echo "      see $OUT"
    fi

    # --- node-tree-api: node tree traversal through the ABI ----------------
    #
    # The node tree group: parse SQL, walk the resulting node tree, and
    # inspect query and plan structures. The assertion that matters is
    # `node_type_query` -- it proves that a parsed statement's node type
    # can be read through the ABI.
    #
    # ON_ERROR_STOP is off: check 12 (the negative control) raises by design.
    echo "  [node-tree-api] node tree traversal against :$PORT"
    OUT=/tmp/kwabi_nodetree_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f node-tree-api.sql postgres >"$OUT" 2>&1

    NT_TRUE=$(grep -cE '^ t *$' "$OUT")
    NT_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    NT_CONTROL=$(grep -c "node tree negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    NT_TYPE=$(grep -A2 'node_type_query' "$OUT" | grep -cE '^ t')
    NT_NAME=$(grep -A2 'node_type_name_query' "$OUT" | grep -cE '^ t')
    NT_CMD=$(grep -A2 'command_type_select' "$OUT" | grep -cE '^ t')

    if [ "$NT_FALSE" -eq 0 ] && [ "$NT_CONTROL" -ge 1 ] && \
       [ "$NT_TYPE" -ge 1 ] && [ "$NT_NAME" -ge 1 ] && [ "$NT_CMD" -ge 1 ]; then
        record PASS "$M" "node-tree-api green ($NT_TRUE assertions)"
    else
        record FAIL "$M" "node-tree-api: true=$NT_TRUE false=$NT_FALSE control=$NT_CONTROL type=$NT_TYPE name=$NT_NAME cmd=$NT_CMD"
        echo "      see $OUT"
    fi

    # --- tuple-api: tuple/slot access through the ABI ---------------------
    #
    # The tuple/slot group: tuple descriptor accessors, heap tuple accessors,
    # and slot accessors. The assertion that matters is `tuple_desc_accessors`
    # -- it proves that tuple_natts, tuple_typeid, tuple_typmod, tuple_attname,
    # tuple_attisdropped, and tuple_attnum all return correct values through
    # the ABI.
    #
    # ON_ERROR_STOP is off: check 5 (the negative control) raises by design.
    echo "  [tuple-api] tuple/slot access against :$PORT"
    OUT=/tmp/kwabi_tuple_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f tuple-api.sql postgres >"$OUT" 2>&1

    TUPLE_TRUE=$(grep -cE '^ t *$' "$OUT")
    TUPLE_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    TUPLE_CONTROL=$(grep -c "tuple negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    TUPLE_DESC=$(grep -A2 'tuple_desc_accessors' "$OUT" | grep -cE '^ t')
    TUPLE_HEAP=$(grep -A2 'heap_tuple_accessors' "$OUT" | grep -cE '^ t')
    TUPLE_SLOT=$(grep -A2 'slot_accessors' "$OUT" | grep -cE '^ t')

    if [ "$TUPLE_FALSE" -eq 0 ] && [ "$TUPLE_CONTROL" -ge 1 ] && \
       [ "$TUPLE_DESC" -ge 1 ] && [ "$TUPLE_HEAP" -ge 1 ] && \
       [ "$TUPLE_SLOT" -ge 1 ]; then
        record PASS "$M" "tuple-api green ($TUPLE_TRUE assertions)"
    else
        record FAIL "$M" "tuple-api: true=$TUPLE_TRUE false=$TUPLE_FALSE control=$TUPLE_CONTROL desc=$TUPLE_DESC heap=$TUPLE_HEAP slot=$TUPLE_SLOT"
        echo "      see $OUT"
    fi

    # --- relation-api: relation cache slots through the ABI ----------------
    #
    # The relation cache group: open a relation, read its metadata (name,
    # namespace, relkind, relam, tupledesc, index list), and close it. The
    # assertion that matters is `relation_open_name` -- it proves that
    # relation_open, relation_name, and relation_close all work together.
    #
    # ON_ERROR_STOP is off: check 13 (the negative control) raises by design.
    echo "  [relation-api] relation cache against :$PORT"
    OUT=/tmp/kwabi_relation_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f relation-api.sql postgres >"$OUT" 2>&1

    REL_TRUE=$(grep -cE '^ t *$' "$OUT")
    REL_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    REL_CONTROL=$(grep -c "relation negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    REL_OPEN=$(grep -A2 'relation_open_name' "$OUT" | grep -cE '^ t')
    REL_ID=$(grep -A2 'relation_id' "$OUT" | grep -cE '^ t')
    REL_KIND=$(grep -A2 'rel_relkind' "$OUT" | grep -cE '^ t')

    if [ "$REL_FALSE" -eq 0 ] && [ "$REL_CONTROL" -ge 1 ] && \
       [ "$REL_OPEN" -ge 1 ] && [ "$REL_ID" -ge 1 ] && [ "$REL_KIND" -ge 1 ]; then
        record PASS "$M" "relation-api green ($REL_TRUE assertions)"
    else
        record FAIL "$M" "relation-api: true=$REL_TRUE false=$REL_FALSE control=$REL_CONTROL open=$REL_OPEN id=$REL_ID kind=$REL_KIND"
        echo "      see $OUT"
    fi

    # --- buffer-lock-api: buffer manager and lock slots through the ABI ---
    #
    # The buffer manager and lock group: read buffers, manage pages,
    # acquire/release LWLocks and spinlocks, and check lock state. The
    # assertion that matters is `buffer_manager` -- it proves that
    # buffer_get, buffer_get_page, buffer_mark_dirty, and buffer_release
    # all work together.
    #
    # ON_ERROR_STOP is off: check 10 (the negative control) raises by design.
    echo "  [buffer-lock-api] buffer manager and locks against :$PORT"
    OUT=/tmp/kwabi_buflock_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f buffer-lock-api.sql postgres >"$OUT" 2>&1

    BUF_TRUE=$(grep -cE '^ t *$' "$OUT")
    BUF_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    BUF_CONTROL=$(grep -c "lock negative control fired as intended" "$OUT")
    # The three that carry the meaning, so a failure says which.
    BUF_MGR=$(grep -A2 'buffer_manager' "$OUT" | grep -cE '^ t')
    BUF_LWLOCK=$(grep -A2 'lwlock_lifecycle' "$OUT" | grep -cE '^ t')
    BUF_SPIN=$(grep -A2 'spinlock_lifecycle' "$OUT" | grep -cE '^ t')

    if [ "$BUF_FALSE" -eq 0 ] && [ "$BUF_CONTROL" -ge 1 ] && \
       [ "$BUF_MGR" -ge 1 ] && [ "$BUF_LWLOCK" -ge 1 ] && \
       [ "$BUF_SPIN" -ge 1 ]; then
        record PASS "$M" "buffer-lock-api green ($BUF_TRUE assertions)"
    else
        record FAIL "$M" "buffer-lock-api: true=$BUF_TRUE false=$BUF_FALSE control=$BUF_CONTROL mgr=$BUF_MGR lwlock=$BUF_LWLOCK spin=$BUF_SPIN"
        echo "      see $OUT"
    fi

    # --- lock-api: lock slots through the ABI -----------------------------
    #
    # The lock group: acquire, release, and check LWLocks and spinlocks.
    # The assertion that matters is `lock_lifecycle` — it proves that
    # lock_acquire, lock_held_by_me, and lock_release all work together.
    #
    # ON_ERROR_STOP is off: check 5 (the negative control) raises by design.
    echo "  [lock-api] lock slots against :$PORT"
    OUT=/tmp/kwabi_lock_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f lock-api.sql postgres >"$OUT" 2>&1

    LOCK_TRUE=$(grep -cE '^ t *$' "$OUT")
    LOCK_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    LOCK_CONTROL=$(grep -c "lock negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    LOCK_LIFECYCLE=$(grep -A2 'lock_lifecycle' "$OUT" | grep -cE '^ t')

    if [ "$LOCK_FALSE" -eq 0 ] && [ "$LOCK_CONTROL" -ge 1 ] && \
       [ "$LOCK_LIFECYCLE" -ge 1 ]; then
        record PASS "$M" "lock-api green ($LOCK_TRUE assertions)"
    else
        record FAIL "$M" "lock-api: true=$LOCK_TRUE false=$LOCK_FALSE control=$LOCK_CONTROL lifecycle=$LOCK_LIFECYCLE"
        echo "      see $OUT"
    fi

    # --- stringinfo-api: stringinfo slots through the ABI -----------------
    #
    # The stringinfo group: create, append, read, reset. The assertion that
    # matters is `stringinfo_basic` — it proves that stringinfo_init,
    # stringinfo_append, stringinfo_append_char, stringinfo_append_int,
    # stringinfo_data, stringinfo_len, and stringinfo_reset all work together.
    #
    # ON_ERROR_STOP is off: check 3 (the negative control) raises by design.
    echo "  [stringinfo-api] stringinfo slots against :$PORT"
    OUT=/tmp/kwabi_stringinfo_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f stringinfo-api.sql postgres >"$OUT" 2>&1

    SI_TRUE=$(grep -cE '^ t *$' "$OUT")
    SI_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    SI_CONTROL=$(grep -c "stringinfo negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    SI_BASIC=$(grep -A2 'stringinfo_basic' "$OUT" | grep -cE '^ t')

    if [ "$SI_FALSE" -eq 0 ] && [ "$SI_CONTROL" -ge 1 ] && \
       [ "$SI_BASIC" -ge 1 ]; then
        record PASS "$M" "stringinfo-api green ($SI_TRUE assertions)"
    else
        record FAIL "$M" "stringinfo-api: true=$SI_TRUE false=$SI_FALSE control=$SI_CONTROL basic=$SI_BASIC"
        echo "      see $OUT"
    fi

    # --- shmem-api: shmem slots through the ABI ---------------------------
    #
    # The shmem group: allocate, write, read, free shared memory. The
    # assertion that matters is `shmem_basic` — it proves that shmem_alloc,
    # shmem_free, and shmem_get all work together.
    #
    # ON_ERROR_STOP is off: check 2 (the negative control) raises by design.
    echo "  [shmem-api] shmem slots against :$PORT"
    OUT=/tmp/kwabi_shmem_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f shmem-api.sql postgres >"$OUT" 2>&1

    SH_TRUE=$(grep -cE '^ t *$' "$OUT")
    SH_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    SH_CONTROL=$(grep -c "shmem negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    SH_BASIC=$(grep -A2 'shmem_basic' "$OUT" | grep -cE '^ t')

    if [ "$SH_FALSE" -eq 0 ] && [ "$SH_CONTROL" -ge 1 ] && \
       [ "$SH_BASIC" -ge 1 ]; then
        record PASS "$M" "shmem-api green ($SH_TRUE assertions)"
    else
        record FAIL "$M" "shmem-api: true=$SH_TRUE false=$SH_FALSE control=$SH_CONTROL basic=$SH_BASIC"
        echo "      see $OUT"
    fi

    # --- syscache-api: syscache slots through the ABI ----------------------
    #
    # The syscache group: look up system catalog entries. The assertion that
    # matters is `syscache_basic` — it proves that syscache_get_oid,
    # syscache_get_tuple, and syscache_free_tuple all work together.
    #
    # ON_ERROR_STOP is off: check 2 (the negative control) raises by design.
    echo "  [syscache-api] syscache slots against :$PORT"
    OUT=/tmp/kwabi_syscache_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f syscache-api.sql postgres >"$OUT" 2>&1

    SC_TRUE=$(grep -cE '^ t *$' "$OUT")
    SC_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    SC_CONTROL=$(grep -c "syscache negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    SC_BASIC=$(grep -A2 'syscache_basic' "$OUT" | grep -cE '^ t')

    if [ "$SC_FALSE" -eq 0 ] && [ "$SC_CONTROL" -ge 1 ] && \
       [ "$SC_BASIC" -ge 1 ]; then
        record PASS "$M" "syscache-api green ($SC_TRUE assertions)"
    else
        record FAIL "$M" "syscache-api: true=$SC_TRUE false=$SC_FALSE control=$SC_CONTROL basic=$SC_BASIC"
        echo "      see $OUT"
    fi

    # --- extension-api: extension slots through the ABI -------------------
    #
    # The extension group: look up extension metadata. The assertion that
    # matters is `extension_basic` — it proves that extension_oid,
    # extension_installed, and extension_version all work together.
    #
    # ON_ERROR_STOP is off: check 2 (the negative control) raises by design.
    echo "  [extension-api] extension slots against :$PORT"
    OUT=/tmp/kwabi_extension_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f extension-api.sql postgres >"$OUT" 2>&1

    EX_TRUE=$(grep -cE '^ t *$' "$OUT")
    EX_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    EX_CONTROL=$(grep -c "extension negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    EX_BASIC=$(grep -A2 'extension_basic' "$OUT" | grep -cE '^ t')

    if [ "$EX_FALSE" -eq 0 ] && [ "$EX_CONTROL" -ge 1 ] && \
       [ "$EX_BASIC" -ge 1 ]; then
        record PASS "$M" "extension-api green ($EX_TRUE assertions)"
    else
        record FAIL "$M" "extension-api: true=$EX_TRUE false=$EX_FALSE control=$EX_CONTROL basic=$EX_BASIC"
        echo "      see $OUT"
    fi

    # --- transaction-api: the transaction accessor through the ABI ---------
    #
    # The transaction group has ONE slot now: transaction_get_current_xid. An
    # extension reached from SQL is already inside a transaction and cannot
    # start/commit/abort one, so those slots were removed (see kwabi.h
    # Transactions). Two assertions carry the meaning:
    #   * transaction_lifecycle  — a write assigns an XID and the accessor sees
    #                              a non-zero id;
    #   * unassigned_xid_is_zero — the accessor is a PURE READ: a read-only
    #                              transaction reports 0 (it does not allocate).
    #
    # ON_ERROR_STOP is off: the negative control raises by design.
    echo "  [transaction-api] transaction accessor against :$PORT"
    OUT=/tmp/kwabi_transaction_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f transaction-api.sql postgres >"$OUT" 2>&1

    TXN_TRUE=$(grep -cE '^ t *$' "$OUT")
    TXN_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    TXN_CONTROL=$(grep -c "transaction negative control fired as intended" "$OUT")
    # The two that carry the meaning.
    TXN_LIFECYCLE=$(grep -A2 'transaction_lifecycle' "$OUT" | grep -cE '^ t')
    TXN_PURE=$(grep -A2 'unassigned_xid_is_zero' "$OUT" | grep -cE '^ t')

    if [ "$TXN_FALSE" -eq 0 ] && [ "$TXN_CONTROL" -ge 1 ] && \
       [ "$TXN_LIFECYCLE" -ge 1 ] && [ "$TXN_PURE" -ge 1 ]; then
        record PASS "$M" "transaction-api green ($TXN_TRUE assertions)"
    else
        record FAIL "$M" "transaction-api: true=$TXN_TRUE false=$TXN_FALSE control=$TXN_CONTROL lifecycle=$TXN_LIFECYCLE pure=$TXN_PURE"
        echo "      see $OUT"
    fi

    # --- hook-api: the executor hook chain through the ABI -----------------
    #
    # Hooks are per backend and cannot be removed, so hook-api.sql runs in its own
    # session and installs its bodies once. Each check prints "check <name>: t|f".
    echo "  [hook-api] executor hook chain against :$PORT"
    OUT=/tmp/kwabi_hook_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f hook-api.sql postgres >"$OUT" 2>&1

    HOOK_TRUE=$(grep -cE 'check .*: t$' "$OUT")
    HOOK_FALSE=$(grep -cE 'check .*: f$' "$OUT")

    if [ "$HOOK_FALSE" -eq 0 ] && [ "$HOOK_TRUE" -ge 7 ]; then
        record PASS "$M" "hook-api green ($HOOK_TRUE checks)"
    else
        record FAIL "$M" "hook-api: true=$HOOK_TRUE false=$HOOK_FALSE"
        echo "      see $OUT"
    fi

    # --- executor-api: executor slots through the ABI ----------------------
    #
    # The executor group: start, run, getnext, finish, end. The assertion that
    # matters is `executor_lifecycle` — it proves that executor_start,
    # executor_run, executor_getnext, executor_finish, and executor_end all
    # work together.
    #
    # ON_ERROR_STOP is off: check 3 (the negative control) raises by design.
    echo "  [executor-api] executor slots against :$PORT"
    OUT=/tmp/kwabi_executor_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f executor-api.sql postgres >"$OUT" 2>&1

    EXEC_TRUE=$(grep -cE '^ t *$' "$OUT")
    EXEC_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    EXEC_CONTROL=$(grep -c "executor negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    EXEC_LIFECYCLE=$(grep -A2 'executor_lifecycle' "$OUT" | grep -cE '^ t')

    if [ "$EXEC_FALSE" -eq 0 ] && [ "$EXEC_CONTROL" -ge 1 ] && \
       [ "$EXEC_LIFECYCLE" -ge 1 ]; then
        record PASS "$M" "executor-api green ($EXEC_TRUE assertions)"
    else
        record FAIL "$M" "executor-api: true=$EXEC_TRUE false=$EXEC_FALSE control=$EXEC_CONTROL lifecycle=$EXEC_LIFECYCLE"
        echo "      see $OUT"
    fi

    # --- explain-api: explain slots through the ABI ------------------------
    #
    # The explain group: explain_get_index_name. The assertion that matters is
    # `index_name` — it proves that explain_get_index_name works through the ABI.
    #
    # ON_ERROR_STOP is off: check 3 (the negative control) raises by design.
    echo "  [explain-api] explain slots against :$PORT"
    OUT=/tmp/kwabi_explain_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f explain-api.sql postgres >"$OUT" 2>&1

    EXP_TRUE=$(grep -cE '^ t *$' "$OUT")
    EXP_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    EXP_CONTROL=$(grep -c "explain negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    EXP_INDEX=$(grep -A2 'index_name' "$OUT" | grep -cE '^ t')

    if [ "$EXP_FALSE" -eq 0 ] && [ "$EXP_CONTROL" -ge 1 ] && \
       [ "$EXP_INDEX" -ge 1 ]; then
        record PASS "$M" "explain-api green ($EXP_TRUE assertions)"
    else
        record FAIL "$M" "explain-api: true=$EXP_TRUE false=$EXP_FALSE control=$EXP_CONTROL index=$EXP_INDEX"
        echo "      see $OUT"
    fi

    # --- bgworker-api: background worker slots through the ABI ------------
    #
    # The bgworker group: register, check running, terminate. The assertion
    # that matters is `bgworker_lifecycle` — it proves that bgworker_register,
    # bgworker_is_running, and bgworker_terminate all work together.
    #
    # ON_ERROR_STOP is off: check 2 (the negative control) raises by design.
    echo "  [bgworker-api] background worker slots against :$PORT"
    OUT=/tmp/kwabi_bgworker_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
        -f bgworker-api.sql postgres >"$OUT" 2>&1

    BG_TRUE=$(grep -cE '^ t *$' "$OUT")
    BG_FALSE=$(grep -cE '^ f *$' "$OUT")
    # The control must have RAISED. Its message is the proof it fired.
    BG_CONTROL=$(grep -c "bgworker negative control fired as intended" "$OUT")
    # The one that carries the meaning.
    BG_LIFECYCLE=$(grep -A2 'bgworker_lifecycle' "$OUT" | grep -cE '^ t')

    if [ "$BG_FALSE" -eq 0 ] && [ "$BG_CONTROL" -ge 1 ] && \
       [ "$BG_LIFECYCLE" -ge 1 ]; then
        record PASS "$M" "bgworker-api green ($BG_TRUE assertions)"
    else
        record FAIL "$M" "bgworker-api: true=$BG_TRUE false=$BG_FALSE control=$BG_CONTROL lifecycle=$BG_LIFECYCLE"
        echo "      see $OUT"
    fi

    # --- structured error channel: cross-version safety ------------------
    #
    # A v1 caller's smaller struct must not be overrun by a v2 writer. This is
    # the only test that proves the `size` protocol, and the protocol is what
    # makes growing the error struct survivable at all. The guard band in
    # errsize.c is the assertion; `overrun_bytes=0` is the result.
    echo "  [errsize] cross-version error channel against :$PORT"
    OUT=/tmp/kwabi_errsize_$M.log
    "$PGB/psql" -h "$PSOCK" -p "$PORT" -v ON_ERROR_STOP=1 \
        -v module="errsize.$DLSUFFIX" \
        -f ../errsize/errsize.sql postgres >"$OUT" 2>&1

    ERR_V1=$(grep -c "v1 caller: .*overrun_bytes=0" "$OUT")
    ERR_V2=$(grep -c "v2 caller: overrun_bytes=0" "$OUT")
    ERR_ASSERT=$(grep -c "^ t  *| t  *| t  *| t" "$OUT")

    if [ "$ERR_V1" -ge 1 ] && [ "$ERR_V2" -ge 1 ] && [ "$ERR_ASSERT" -ge 1 ]; then
        record PASS "$M" "error channel size protocol green"
    else
        record FAIL "$M" "errsize: v1=$ERR_V1 v2=$ERR_V2 asserts=$ERR_ASSERT"
        echo "      see $OUT"
    fi

    # --- slru-api: the SLRU group, against its OWN preload cluster ----------
    #
    # SLRU is the one group that cannot use the matrix server. It needs the
    # runtime in shared_preload_libraries and an SLRU declared in kwabi.slrus,
    # both PGC_POSTMASTER. This stage starts a private cluster, runs the test,
    # and tears it down, so the matrix server's environment is untouched.
    #
    # The assertions that carry meaning, beyond "no false":
    #   * the page round-trip (read_back_matches) — real disk I/O, not just a
    #     shared-memory buffer;
    #   * BOTH negative controls must RAISE (an undeclared name; a buffer-count
    #     mismatch). A control that returns a row means the create path is
    #     vacuous.
    #   * the SLRU capability bit must be SET here. capabilities.sql asserts it
    #     is CLEAR on the non-preloaded matrix server; the pair is the two-way
    #     control for the bit tracking the load model.
    SLRU_PORT=$(slru_port_for "$M")
    SLRU_DATA=$(slru_data_for "$M")
    echo "  [slru-api] SLRU group against a preload cluster on :$SLRU_PORT"

    rm -rf "$SLRU_DATA"
    LC_ALL="en_US.UTF-8" LANG="en_US.UTF-8" \
        "$PGB/initdb" -D "$SLRU_DATA" -U "$(whoami)" \
        --encoding=UTF8 --locale=C >/dev/null 2>&1
    printf "shared_preload_libraries = 'kwabi_runtime_pg%s.%s'\nkwabi.slrus = 'kwabitest'\nport = %s\nlisten_addresses = 'localhost'\nunix_socket_directories = '%s'\n" \
        "$M" "$DLSUFFIX" "$SLRU_PORT" "$PSOCK" > "$SLRU_DATA/postgresql.auto.conf"

    OUT=/tmp/kwabi_slru_$M.log
    SLRU_STARTED=0
    if LC_ALL="en_US.UTF-8" LANG="en_US.UTF-8" \
        "$PGB/pg_ctl" -D "$SLRU_DATA" -l "$SLRU_DATA/server.log" -w start >/dev/null 2>&1; then
        SLRU_STARTED=1
    fi

    if [ $SLRU_STARTED -eq 0 ]; then
        record FAIL "$M" "slru-api: preload cluster would not start"
        echo "      see $SLRU_DATA/server.log"
        tail -3 "$SLRU_DATA/server.log" 2>/dev/null | sed 's/^/      /'
    else
        "$PGB/psql" -h "$PSOCK" -p "$SLRU_PORT" -v ON_ERROR_STOP=0 \
            -v bundle="kwabi_runtime_pg$M.$DLSUFFIX" \
            -f slru-api.sql postgres >"$OUT" 2>&1

        SLRU_TRUE=$(grep -cE '^ t *$' "$OUT")
        SLRU_FALSE=$(grep -cE '^ f *$' "$OUT")
        # Both controls must have RAISED. Their messages are the proof.
        SLRU_CTL1=$(grep -c "was not declared" "$OUT")
        SLRU_CTL2=$(grep -c "declared with 16 buffers" "$OUT")
        # The assertions that carry the meaning.
        SLRU_RT=$(grep -A2 'read_back_matches' "$OUT" | grep -cE '^ t')
        SLRU_CAPBIT=$(grep -A2 'slru_bit_set_when_preloaded' "$OUT" | grep -cE '^ t')

        if [ "$SLRU_FALSE" -eq 0 ] && [ "$SLRU_CTL1" -ge 1 ] && \
           [ "$SLRU_CTL2" -ge 1 ] && [ "$SLRU_RT" -ge 1 ] && \
           [ "$SLRU_CAPBIT" -ge 1 ]; then
            record PASS "$M" "slru-api green ($SLRU_TRUE assertions, preload cluster)"
        else
            record FAIL "$M" "slru-api: true=$SLRU_TRUE false=$SLRU_FALSE ctl1=$SLRU_CTL1 ctl2=$SLRU_CTL2 roundtrip=$SLRU_RT capbit=$SLRU_CAPBIT"
            echo "      see $OUT"
        fi

        LC_ALL="en_US.UTF-8" LANG="en_US.UTF-8" \
            "$PGB/pg_ctl" -D "$SLRU_DATA" -w stop >/dev/null 2>&1 || true
    fi
    rm -rf "$SLRU_DATA"
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo
hr
echo "results"
hr
for r in "${RESULTS[@]}"; do echo "  $r"; done
hr
echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"

if [ $FAIL -gt 0 ]; then
    echo "MATRIX FAILED"
    exit 1
fi
echo "MATRIX PASSED"
