# POC + local CI: kwabi inside live PostgreSQL 16, 17 and 18

`make ci` — one extension binary, three servers, 13/13 green on each.

This is the step from "the mechanism works against a stub" to "the mechanism
works against PostgreSQL", and then to the claim the whole project rests on:
**an extension built once runs on every supported major version.**

## Run it

```sh
cd notes/runtime-skeleton/shim

make ci                    # the local matrix: core + canary + 16/17/18
make build-once            # the same, without the core checks
make proof PG=18           # 13 checks against the PostgreSQL 18.6 on :5432
make proof PG=17           # the same against 17.11 on :5433
make proof PG=16           # the same against 16.15 on :5434
make uninstall             # remove both objects from every $libdir
```

Setup, once:

```sh
brew install postgresql@16 postgresql@17   # keg-only: do not shadow @18
make cluster-start                         # local clusters on :5434 and :5433
make cluster-status
```

## Build-once, demonstrated

The canary is built **once** from `../canary/canary.c`. Its `make` target does
not depend on `PG`, and it is installed unchanged into every `$libdir`.

```
=== canary hash ===
d8907e177a14445fbedb8c7e954234d69e11054f7192ce5a2cde40119380953c
```

| | PG 16.15 (:5434) | PG 17.11 (:5433) | PG 18.6 (:5432) |
|---|---|---|---|
| ABI reports | `…bound to PostgreSQL 1600` | `…bound to PostgreSQL 1700` | `…bound to PostgreSQL 1800` |
| canary sha256 | `d8907e17…` | `d8907e17…` | `d8907e17…` |
| runtime sha256 | `3b88722a…` | `06d67de3…` | `9a82b837…` |
| all 13 checks | pass | pass | pass |

The three **installed canaries are byte-identical**; the three runtimes are not:

```
d8907e17…  /opt/homebrew/opt/postgresql@16/lib/postgresql/libcanary.dylib
d8907e17…  /opt/homebrew/lib/postgresql@17/libcanary.dylib
d8907e17…  /opt/homebrew/lib/postgresql@18/libcanary.dylib    <- all one file
3b88722a…  …/postgresql@16/lib/postgresql/kwabi_runtime_pg16.dylib
06d67de3…  /opt/homebrew/lib/postgresql@17/kwabi_runtime_pg17.dylib
9a82b837…  /opt/homebrew/lib/postgresql@18/kwabi_runtime_pg18.dylib
```

That is the architecture, visible as two sets of hashes: **the extension is one
artifact; the runtime is one per major version.** The rebuild is reproducible —
same source gives the same hash — so the canary can be hash-pinned in CI
exactly as `build-once-gate` requires.

## The local CI matrix

`./ci-local.sh` (or `make ci`) runs three stages in order:

| Stage | What it asserts |
|---|---|
| core | `cargo test`; `src/abi.rs` matches `kwabi.h`; `kwabi.h` compiles standalone |
| canary | builds the extension once, pins its sha256, asserts zero undefined symbols |
| matrix | per major: shim compiles → canary hash unchanged → install → the checks in the live server |

The per-major checks are: 13 proof checks, error firewall, panic containment,
capability honesty, capability consumption, and the error-channel size
protocol. Two of those are worth separating out, because they look similar and
are not:

| check | what it proves |
|---|---|
| `capabilities.sql` | the bitset is HONEST — every claimed bit proven behaviourally, no undefined bit set. A fact about the **runtime**. |
| `capability-consumer.sql` | the bitset is REACHABLE and BRANCHED ON — an extension reads it through the SDK, and refuses a guarantee the runtime does not offer. A fact about the **consumer**. |

The second exists because the first was passing while the SDK's mirror of
`KwabiV1` was six appended slots behind `kwabi.h`, so no Rust extension could
reach `api->capabilities` at all. Its load-bearing assertion is that the shim
and the extension agree on the bitset by two independent routes; that is what
catches mirror drift, and no runtime-side check can see it. See
`notes/capabilities-design.md` §7.

The ordering matters. The canary hash is pinned *before* any major is built, so
if any major's build needs its own copy of the extension, the matrix fails on
that major rather than quietly succeeding.

The per-major check counts specific evidence rather than trusting an exit code:
one `kwabi ABI v1` line, two "genuine PostgreSQL memory" results, two "foreign
extension reached" results, one "extension loaded through the ABI", one
`SQLSTATE 22012`, and **exactly one** `ERROR:` (the deliberate raise in check 7).
More errors than that means something else broke.

### The gate was tested against deliberate breaks

A gate that has only ever seen green is not evidence. Both failure modes were
injected and confirmed to fail the right cells:

| Injected break | Result |
|---|---|
| `#error` guarded by `PG_VERSION_NUM < 180000` | 16 FAIL, 17 FAIL, 18 PASS |
| `GetMemoryChunkContext` returns NULL below 18 | 16 FAIL (`errs=5`), 17 FAIL, 18 PASS |

The second one is the important case: it compiles cleanly on every major and
only the *live* check catches it. A build-only gate would have passed it.

One finding from building that first break: `PG_VERSION_NUM` is **not defined
until `postgres.h` is included** (it comes from `pg_config.h`). A version guard
placed above `#include "postgres.h"` silently takes the `#else` branch on every
version — which is exactly what happened on the first attempt, and why the
first injected break failed all three cells instead of two. Any guard in this
file must sit after `postgres.h`.

### Scope: local and single-architecture

This is arm64 only, deliberately. Cross-architecture (x86_64) tests a
*different* failure mode — struct layout, alignment and calling convention —
and needs a second toolchain plus a second Postgres build. Mixing it into this
matrix would make a failing cell ambiguous: is it ABI drift across majors, or
an architecture problem? They are separate exercises and the matrix stays
readable by keeping them apart.

Also out of scope here, and tracked separately: `pg_upgrade` with the runtime
installed (`tap-cluster`), hook coexistence with other extensions
(`coexistence`), and per-call overhead (`perf-bench`).

## What was proven

| # | Check | Result |
|---|---|---|
| 1 | Runtime bundle loads into a live backend (`LOAD`) | passes |
| 3 | ABI reports its version and its bound PostgreSQL | passes |
| 4 | Memory from the ABI is **genuine PostgreSQL memory** | passes |
| 5 | 1000 allocations round-trip inside a transaction | passes |
| 6 | Allocations belong to the transaction's context | passes |
| 7 | A real ERROR through the ABI; backend survives | passes |
| 8 | That error carries SQLSTATE 22012, catchable by plpgsql | passes |
| 9 | ABI still works after the error | passes |
| 10 | A **foreign** extension loads through the ABI | passes |
| 11 | That extension reaches this backend's allocator | passes |
| 12 | Whole chain repeats inside a transaction | passes |
| 13 | Backend healthy afterwards | passes |

### Check 4 is the load-bearing one

`GetMemoryChunkContext(p)` returns the memory context that owns a palloc'd
chunk. The check asserts it equals `CurrentMemoryContext`:

```c
KwabiMemoryContext owner = api->memory_chunk_context(p);
if (owner != api->current_memory_context())
    ereport(ERROR, (errmsg("kwabi: ABI pointer belongs to a different context")));
```

If the runtime had secretly used `malloc`, that call would be reading a chunk
header that is not there. It passes, so memory handed out through the table is
memory PostgreSQL allocated, owns, and will free by its normal rules.

### Checks 10–12 are the build-once claim

`libcanary.dylib` is built from `canary.c`, which includes only `kwabi.h` —
no `postgres.h`, no `PG_VERSION_NUM` branches — and `nm -u` reports zero
undefined symbols. PostgreSQL loads the runtime; the runtime loads the
extension and hands it the table; the extension allocates through that table;
PostgreSQL confirms the result is its own memory.

The extension has no idea which server it is in. That is the point.

## The shim is one file, not one per version

`kwabi_runtime_shim.c` compiles for both 17 and 18. Only the *build* differs
(`pg_config`, hence `PGINC` and `PKGLIBDIR`); the source is shared. A copy per
version would drift silently, so version differences live in `#if` guards
marked `VERSION-DIFF` for one-grep discoverability.

The complete set of differences between 17 and 18 in this file:

| Difference | Guard |
|---|---|
| `BackendId` → `ProcNumber` (17) | `#if PG_VERSION_NUM >= 170000` |
| `ExplainState` moved `commands/explain.h` → `commands/explain_state.h` (18) | `#if PG_VERSION_NUM >= 180000` |

Two guards for two majors, and each one is a fact worth reading rather than a
line of boilerplate. Nothing needs a guard for the scan descriptor: PostgreSQL's
real name is `TableScanDesc`, and it is identical in 17 and 18.

## Findings that changed the design

### 1. `load_external_function` enforces `PG_MODULE_MAGIC` — so the runtime must `dlopen`

The most important result. PostgreSQL's own loader, the one behind
`AS 'lib', 'func'`, refuses any library without a magic block:

```
ERROR: incompatible library "libcanary.dylib": missing magic block
HINT:  Extension libraries are required to use the PG_MODULE_MAGIC macro.
```

The magic block records `PG_VERSION_NUM` among other ABI values
(`FUNC_MAX_ARGS`, `INDEX_MAX_KEYS`, `NAMEDATALEN`, `FLOAT8PASSBYVAL`,
`FMGR_ABI_EXTRA`), so **any library carrying one is bound to a single major
version by construction**.

That is exactly the constraint the project exists to remove. An extension that
must be built once and run on 17 and 18 therefore *cannot* carry a magic block,
and so cannot be opened by PostgreSQL's loader. The runtime opens it with raw
`dlopen`/`dlsym` instead.

The inversion is forced, not chosen:

| | magic block | built | loaded by |
|---|---|---|---|
| runtime | **yes** (satisfies PostgreSQL) | per major version | PostgreSQL |
| extension | **no** (must not) | once, ever | the runtime, via `dlopen` |

Both installed runtimes do carry one — `nm -gU … | grep magic` finds
`_Pg_magic_func` in each — and each accepts only its own major version.

### 2. The runtime must be a C bundle, not a plain `.so`

PostgreSQL does not export its symbols to libraries. A module that needs
`palloc` has to be a Mach-O bundle whose undefined symbols are resolved by the
host process:

```
cc -bundle -Wl,-undefined,dynamic_lookup ...
```

`-bundle_loader postgres` also works but binds the runtime to one specific
binary, which would defeat the build-once property. Every stock extension in
`$libdir` is built this way — `auto_explain.dylib` reports
`Mach-O 64-bit bundle arm64` and leaves `_CurrentMemoryContext`,
`_DefineCustomBoolVariable` and friends undefined.

### 3. `MemoryContextSwitchTo` is not a symbol

It is `static inline` in `palloc.h`, assigning to the exported
`CurrentMemoryContext` global and returning the old value. There is no
function to take the address of, so the shim reimplements the two-line body.
`palloc`, `palloc0`, `repalloc`, `pfree`, `GetMemoryChunkContext`,
`MemoryContextStrdup` and the `err*` family **are** exported and bind
normally.

### 4. `BackendId` was renamed to `ProcNumber` in PostgreSQL 17

A concrete, dated instance of the problem this project solves:

```
PG 16.15: storage/backendid.h → typedef int BackendId;
PG 17.11: file deleted       → storage/procnumber.h → typedef int ProcNumber;
```

An extension naming `BackendId` stops compiling at 17. One using the kwabi slot
does not, because the shim absorbs the rename. `BackendId` should be replaced in
a future ABI revision; the shim keeps the name working until then.

### 7. `ScanDesc` was a typo in the ABI, now corrected

The header originally declared a `ScanDesc` handle. No PostgreSQL version has
ever had that type — the real one is `TableScanDesc`
(`access/relscan.h`), and it is byte-identical in 17 and 18. The slot was
renamed to `TableScanDesc` before the ABI was frozen, which is the cheapest
moment to fix it: nothing had been published against the name.

A shim could have papered over it with `typedef void *ScanDesc;`, and an
earlier draft did. That is exactly the wrong fix — it would have hidden a wrong
name behind a compatible-looking alias, and any extension author reaching for
the type would have got something that only existed inside kwabi.

### 5. `PG_MODULE_MAGIC` is a function, not data

`Pg_magic_func()` returns a pointer to a static `Pg_magic_struct`. The backend
uses `dlsym`, which is only guaranteed to work on functions — hence the
function rather than an exported symbol.

### 6. macOS specifics

- **`DLSUFFIX` is `.dylib`** even though the object is a Mach-O *bundle*, so
  the filename must end `.dylib` for `LOAD` and `AS 'lib'` to find it. Naming
  it `.bundle` produces `could not access file`.
- **PostgreSQL 17 will not start without a valid locale** on macOS:
  `FATAL: postmaster became multithreaded during startup`, hint
  `Set the LC_ALL environment variable`. `LC_ALL` must be in the *server's*
  environment — hence `LC_ALL=… pg_ctl start` in the Makefile.
- **`postgresql@17` is keg-only**, so it does not shadow the linked `@18`.
  That is why both can coexist and why `PG_CONFIG` must be given explicitly.

## How the extension side must compile

A kwabi extension includes `kwabi.h` standalone — its aliases stand in for
PostgreSQL's. The shim, which includes `postgres.h`, defines
`KWABI_NO_PG_TYPE_ALIASES` so the real definitions win and the two sets do not
collide. `bgworker_main_type` is suppressed with them because it is
PostgreSQL's own name.

## Layout

| File | Role |
|---|---|
| `kwabi_runtime_shim.c` | the shim: one file, both versions, `VERSION-DIFF` guards |
| `proof.sql` | the 13 checks; takes bundle/canary/libdir as psql variables |
| `capabilities.sql` | the bitset must be honest (runtime side) |
| `capability-consumer.sql` | the bitset must be reachable and branched on (extension side) |
| `guard-test.sql`, `guard-control.sql` | `#[guarded_body]` containment, and the aborting control |
| `try.sql`, `TRY-PROTOTYPE.md` | the `kwabi_try` prototype and its findings |
| `panic-probe.sql` | the panic limitation, isolated (drops the connection) |
| `ci-local.sh` | the local matrix: core, canary pinning, per-major checks |
| `Makefile` | `make ci`, `make build-once`, `make proof PG=16\|17\|18`, `make cluster-start` |
| `../canary/canary.c` | the extension: `kwabi.h` only, zero PG symbols |

The shim links the Rust core as a **static library** and copies the published
table to overlay three slots that must live in C (`memory_chunk_context`,
`current_memory_context`, `raise_error` — the last because it `longjmp`s and
must not be entered from a Rust frame).

## Limits of this POC

- **Three versions.** 16, 17 and 18, arm64 only. PostgreSQL 15 would need a
  fifth guard (`BackendId` still exists, so the `#else` covers it) but is
  untested; x86_64 is a separate exercise.
- **Five slots.** Memory and error handling. The other 195 are null.
- **The shim's `KwabiNative` mirror is hand-maintained.** Eleven fields,
  deliberately, against the 200 generated slots of `KwabiV1`. A real build
  should assert the two agree on size.
- **Not covered:** `pg_upgrade` with the runtime installed, failover, hook
  coexistence with other extensions (`tap-cluster`, `coexistence`).
