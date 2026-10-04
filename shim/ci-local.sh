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

MAJORS_ALL=(16 17 18)

# ---------------------------------------------------------------------------
# Matrix definition
# ---------------------------------------------------------------------------

pg_config_for() {
    case "$1" in
        16) echo "/opt/homebrew/opt/postgresql@16/bin/pg_config" ;;
        17) echo "/opt/homebrew/opt/postgresql@17/bin/pg_config" ;;
        18) echo "pg_config" ;;
    esac
}

pg_bin_for() {
    case "$1" in
        16) echo "/opt/homebrew/opt/postgresql@16/bin" ;;
        17) echo "/opt/homebrew/opt/postgresql@17/bin" ;;
        18) echo "$(pg_config --bindir)" ;;
    esac
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

echo "[core] src/abi.rs matches kwabi.h"
if (cd .. && python3 gen_kwabi_struct.py ../kwabi.h > /tmp/kwabi_abi_check.rs \
      && diff -q /tmp/kwabi_abi_check.rs src/abi.rs >/dev/null); then
    record PASS core "src/abi.rs is current"
else
    record FAIL core "src/abi.rs is stale — run gen_kwabi_struct.py"
fi

echo "[core] header compiles standalone"
if cc -fsyntax-only -Wall -Wextra -x c "$(cd .. && pwd)/../kwabi.h" 2>/dev/null; then
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
CANARY="../canary/libcanary.dylib"

if [ ! -f "$CANARY" ]; then
    echo "FATAL: canary was not built"
    exit 1
fi

CANARY_HASH=$(shasum -a 256 "$CANARY" | cut -d' ' -f1)
echo "  path : $CANARY"
echo "  hash : $CANARY_HASH"

# The guarded-body canary is a second version-independent artifact. It must be
# pinned the same way, or a major could quietly get its own copy.
GUARDED_CANARY="../canary/guarded/target/release/libguarded_canary.dylib"
make -s guard-build >/dev/null 2>&1
if [ ! -f "$GUARDED_CANARY" ]; then
    record FAIL canary "guarded canary was not built"
    exit 1
fi
GUARDED_HASH=$(shasum -a 256 "$GUARDED_CANARY" | cut -d' ' -f1)
echo "  guarded: $GUARDED_HASH"

UNDEF=$(nm -u "$CANARY" 2>/dev/null)
if [ -z "$UNDEF" ]; then
    record PASS canary "zero undefined symbols"
else
    record FAIL canary "has undefined symbols: $UNDEF"
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
        echo "  skip: no pg_config for $M (brew install postgresql@$M)"
        record SKIP "$M" "not installed"
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
    NOW=$(shasum -a 256 "$CANARY" | cut -d' ' -f1)
    if [ "$NOW" != "$CANARY_HASH" ]; then
        record FAIL "$M" "canary was rebuilt — build-once violated"
        continue
    fi
    GNOW=$(shasum -a 256 "$GUARDED_CANARY" | cut -d' ' -f1)
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
    if [ ! -f "$PKGLIB/libguarded_canary.dylib" ]; then
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
    if ! "$PGB/pg_isready" -p "$PORT" -q 2>/dev/null; then
        echo "  no server on :$PORT — attempting to start"
        DATA=$(data_for "$M")
        if [ -f "$DATA/PG_VERSION" ]; then
            LC_ALL="en_US.UTF-8" LANG="en_US.UTF-8" \
                "$PGB/pg_ctl" -D "$DATA" -l "$DATA/server.log" start >/dev/null 2>&1
            sleep 2
        fi
    fi

    if ! "$PGB/pg_isready" -p "$PORT" -q 2>/dev/null; then
        record SKIP "$M" "no server on :$PORT"
        continue
    fi

    # --- the 13 checks ---------------------------------------------------
    echo "  [proof] 13 checks against :$PORT"
    OUT=/tmp/kwabi_proof_$M.log
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.dylib" \
        -v canary="libcanary.dylib" \
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
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.dylib" \
        -v canary="libcanary.dylib" \
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
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.dylib" \
        -v libdir="$PKGLIB" \
        -v guarded="libguarded_canary.dylib" \
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
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=1 \
        -v bundle="kwabi_runtime_pg$M.dylib" \
        -v canary="$PKGLIB/libguarded_canary.dylib" \
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
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=1 \
        -v bundle="kwabi_runtime_pg$M.dylib" \
        -v libdir="$PKGLIB" \
        -v guarded="libguarded_canary.dylib" \
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
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.dylib" \
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
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=0 \
        -v bundle="kwabi_runtime_pg$M.dylib" \
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

    # --- structured error channel: cross-version safety ------------------
    #
    # A v1 caller's smaller struct must not be overrun by a v2 writer. This is
    # the only test that proves the `size` protocol, and the protocol is what
    # makes growing the error struct survivable at all. The guard band in
    # errsize.c is the assertion; `overrun_bytes=0` is the result.
    echo "  [errsize] cross-version error channel against :$PORT"
    OUT=/tmp/kwabi_errsize_$M.log
    "$PGB/psql" -p "$PORT" -v ON_ERROR_STOP=1 \
        -v module='errsize.dylib' \
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
