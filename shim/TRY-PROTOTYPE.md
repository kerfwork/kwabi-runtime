# kwabi_try prototype

Answers the open question in `notes/error-firewall-design.md` §8: how is the
guarded body represented so no `longjmp` crosses Rust?

## Run

```sh
cd notes/runtime-skeleton/shim
make try          PG=18     # all checks except the panic case
make panic-probe  PG=18     # the panic limitation, isolated (drops the connection)
```

## The answer

The body is a **plain C function pointer plus a `void *`** — not a Rust closure:

```c
typedef KwabiStatus (*KwabiBodyFn)(void *arg);   /* extern "C" */
```

A Rust closure was the obvious alternative and is wrong: a closure has to be
called from Rust, which puts a Rust frame inside the guarded region — the one
thing §2 forbids. A C function pointer can be called directly from the shim's
`PG_TRY` block with no Rust frame between the guard and the body.

The contract on the body:

| rule | why |
|---|---|
| must not **raise** a PostgreSQL error | the jump would cross Rust frames |
| must not **panic** | a panic from a separately built extension cannot be caught (§4) |
| signals failure by **returning** non-OK | so failure travels by value, not by unwinding |

## Measured results

Identical on PostgreSQL 16.15, 17.11 and 18.6.

| check | result |
|---|---|
| body succeeds | `status=0 body_ran=1` |
| body reports failure without raising | `status=1` |
| **body writes a row, then fails** | `survivors=0` — work undone |
| body raises (contract violation) | `status=1 sqlstate=22012` — survivable |
| nested try, inner fails | `outer_status=0 inner_survivors=0` — no leak |
| 20 sequential tries | 20 calls, 0 leaked rows |
| inside an explicit transaction, then commit | 0 rows after commit |
| body panics | **aborts the postmaster** — see below |

## The panic finding, and the design correction

The design originally specified `extern "C-unwind"` on the body, so a panic
could travel out of the body and into the runtime's `catch_unwind`. The
prototype showed that is **wrong**, by taking down the server:

```
thread '<unnamed>' panicked at panic_body.rs:12:5:
canary: deliberate panic from a guarded body
fatal runtime error: Rust cannot catch foreign exceptions, aborting
LOG:  client backend (PID 8964) was terminated by signal 6: Abort trap: 6
LOG:  terminating any other active server processes
LOG:  database system was not properly shut down; automatic recovery in progress
```

**"Rust cannot catch foreign exceptions."** A panic is identified by its Rust
type, and type identity is per-std-instance. The extension is a separately built
artifact with its own copy of Rust std, so its panic is foreign to the runtime's
std and `catch_unwind` refuses it — then aborts anyway.

Reproduced outside PostgreSQL: two Rust dylibs, `dlopen`, same message, same
abort. It is not a PostgreSQL interaction.

So the body is `extern "C"`, and a panic escaping it aborts at the body's own
boundary. Same abort, but honest: the guard does not appear to work when it
cannot.

### Blast radius

A panicking body aborts **the whole postmaster**, not one backend. PostgreSQL
treats a SIGABRT in any backend as a shared-state risk, restarts, and runs crash
recovery. An extension that panics is a cluster-wide event.

That is the argument for the contract being enforced rather than trusted.

## What the prototype leaves open

- **No mechanism enforces "must not panic."** It is a contract. A body that
  panics aborts. Worth considering whether a build-time lint or a
  `catch_unwind` in the *extension's own* crate (where its std can catch its own
  panic) should be the recommended pattern.
- **The body cannot report structured failure.** It returns a status; the
  runtime fills `KwabiError`. An extension that wants to return its own message
  has no way to do so yet. `kwabi_error_get` reads the runtime's buffer, not the
  extension's.
- **Prototype-only slots** (`test_insert`, `test_raise_contained`) exist to
  exercise this end to end and should be removed before the freeze.
