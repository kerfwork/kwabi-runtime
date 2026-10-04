# runtime-skeleton — first proof of operation

The runtime is the per-PostgreSQL-major-version library that publishes the
stable ABI. This is the smallest version of it that proves the handoff works
end to end.

## The claim

An extension compiled against `kwabi.h` reaches PostgreSQL internals without
linking a single PostgreSQL symbol, so the **same extension binary** loads on
every supported major version.

That is the property the whole project exists to provide. Everything else in
the plan — hooks, table AM, logical decoding, the types pack, the shared engine
— is more surface area on this one mechanism.

## Two tables

```
extension.so ──calls──▶ KwabiV1   (stable, frozen, version-tagged, 197 slots)
                           ▲  published once at init
                           │
kwabi_runtime.so ──────────┘
       │ binds at init
       ▼
KwabiNative  (unstable: one build per PostgreSQL major version)
```

| | `KwabiV1` | `KwabiNative` |
|---|---|---|
| Audience | extension authors | runtime only |
| Stability | frozen; append-only | tracks PostgreSQL, breaks freely |
| Built | once, ever | once per major version (16/17/18) |
| Layout | 197 pointer-sized fields | ~11 fields, grows as groups are wired |

The split is the entire design. `KwabiV1` is small enough to hold still;
`KwabiNative` absorbs every struct-layout change PostgreSQL makes. The
4-version header diff (`notes/version-diff-report.md`) showed that
extension-facing APIs are stable while internal structs are volatile — this
architecture is what turns that observation into a guarantee.

## What is wired

Only the memory and error-firewall groups. Every other slot in the published
table stays **null**, which is how this ABI says "not implemented yet". An
extension that tests the slot sees a clean absence instead of jumping into a
stale address.

| Group | Slots | Status |
|---|---|---|
| Memory (`palloc`, `palloc0`, `repalloc`, `pfree`, contexts) | 6 | wired |
| Error firewall (`error_message`, `error_code`, `error_clear`, `ereport`, `elog`) | 5 | wired |
| fmgr, SPI, types, parser, hooks, bgworker, logical decoding, node IR, table AM, … | 186 | null |

Wiring the remaining 186 slots is the rest of the plan. The mechanism does not
change — `build_table()` grows one group at a time.

## The error firewall

PostgreSQL reports errors with `ereport`, which `longjmp`s back up the stack.
A `longjmp` that crosses a Rust frame skips destructors, which is undefined
behaviour. So the ABI never lets one through:

- **Outward (PostgreSQL → extension).** The runtime catches errors and returns
  status codes; the extension reads `error_code()` / `error_message()`.
- **Inward (extension → PostgreSQL).** The extension formats the message
  itself and hands the finished string to `ereport`. No variadic macro crosses
  the boundary — which is also why `ereport`'s third parameter is a varargs
  slot the runtime ignores.

`kwabi_raise()` exercises the first direction: the runtime can fail a call
without raising a PostgreSQL error at all.

## Run it

```sh
cd notes/runtime-skeleton

# Rust-side: layout, publication, null slots, error firewall
cargo test

# Offline end-to-end: a C extension in a separate .so, driven through the ABI
cargo build --release
make proof

# Live POC: the same extension inside real PostgreSQL 17 and 18
cd shim && make build-once
```

`make proof` here is the offline harness — a stub PostgreSQL in-process.
`cd shim && make build-once` is the real thing: two servers, one canary.
See `shim/README.md`.

## Files

| File | What it is |
|---|---|
| `src/abi.rs` | **generated** from `../kwabi.h` by `gen_kwabi_struct.py`; do not edit |
| `src/lib.rs` | runtime: `KwabiNative`, the wrappers, `kwabi_runtime_init` |
| `gen_kwabi_struct.py` | parses `kwabi.h` and emits `src/abi.rs` |
| `tests/handoff.rs` | Rust-side tests against a stub PostgreSQL |
| `canary/canary.c` | a C extension that includes only `kwabi.h` |
| `canary/host.c` | stands in for PostgreSQL: supplies natives, loads the canary, drives it |
| `bindgen.sh` | per-version bindgen matrix (deferred to CI; no bindgen on this macOS) |

### Why the struct is generated

`KwabiV1` has 197 fields. Hand-copying them into Rust guarantees drift, and
drift here is memory corruption, not a compile error. So the header stays the
single source of truth and `gen_kwabi_struct.py` derives the Rust mirror.
`table_layout_is_stable` then asserts `size_of::<KwabiV1>() == FIELD_COUNT * 8`
and pins the count at 197, so an edit to either side fails a test.

Generating it immediately caught a real defect: every handle typedef in
`kwabi.h` is `typedef void *KwabiRelation;`, and the struct then declared
`KwabiRelation *` at 111 use sites — a `void **`. Those are fixed; the header
now compiles clean under `gcc -fsyntax-only -Wall -Wextra`.

## Verified output

Rust side:

```
running 6 tests
test init_publishes_a_usable_table ... ok
test init_rejects_a_null_native_table ... ok
test table_layout_is_stable ... ok
test extension_reaches_stub_postgres_through_the_table ... ok
test unimplemented_slots_are_null_not_garbage ... ok
test error_firewall_keeps_runtime_errors_out_of_postgres ... ok
```

The canary is built with no PostgreSQL headers and reports **zero undefined
symbols**:

```
$ nm -u libcanary.dylib
(empty)
```

The handoff, end to end:

```
[2] load the prebuilt extension
  PASS dlopen succeeded
[3] hand the table to the extension
  PASS kwabi_ext_init accepted the table
[4] drive the extension through the ABI
  PASS canary_alloc returned memory
  PASS the call reached the host's allocator
  PASS extension wrote its marker into that memory
[5] unimplemented slots are null, not garbage
  PASS fmgr_info is null (not yet implemented)
[6] error firewall
  PASS runtime message wins
  PASS clear restores passthrough
ALL CHECKS PASSED
```

## What this does not prove

The native table in the harness is a stub, so this proves the **handoff
mechanism**, not that the runtime's per-version bindings match real PostgreSQL.

That gap is now closed by the live POC — `shim/README.md` — which loads the
same canary into real PostgreSQL 17.11 and 18.6 and passes all 13 checks on
both. What remains unproven:

- PostgreSQL 16 (the shim's `#else` branch covers it; untested) — `ci-matrix`;
- hook coexistence with other extensions — `coexistence`;
- `pg_upgrade` with the runtime installed — `tap-cluster`;
- overhead per call across the ABI boundary — `perf-bench`.

## Findings worth carrying forward

- **Variadic definitions are not stable Rust.** `ereport`/`elog` cannot be
  forwarded as variadic Rust functions. The ABI's answer — pre-formatted
  message, varargs slot ignored — is forced, and it happens to be the right
  design anyway, because it is also what keeps `longjmp` out of the boundary.
- **Table address stability is load-bearing.** Extensions cache the pointer,
  so `kwabi_runtime_init` uses `OnceLock::get_or_init` and returns the same
  address to every caller. The first version returned null under a concurrent
  second call, which the parallel test run exposed.
- **Null slots are the capability mechanism for v1.** No bitset is needed
  until there is a slot an extension might want to call *optionally*. A null
  check is enough while the table is mostly empty.
