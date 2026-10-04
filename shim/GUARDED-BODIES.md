# Guarded bodies: making "must not panic" true

Answers the open question from `notes/error-firewall-design.md` §4: the contract
said a body must not panic, but nothing enforced it, and a body that panicked
aborted the whole postmaster.

## The mechanism

Catch the panic **in the extension's own crate**. The runtime cannot — the
extension's panic is a foreign exception to the runtime's Rust std — but the
extension's own std can catch its own panic. That is the entire trick.

`#[guarded_body]` generates the trampoline that does it:

```rust
use kwabi::guarded_body;

#[repr(C)]
pub struct Arg { pub n: i32 }

#[guarded_body]
fn attempt(arg: &mut Arg) -> Result<(), String> {
    if arg.n < 0 { panic!("negative"); }   // contained -> KWABI_ERR_PANICKED
    Ok(())
}
// generates: extern "C" fn attempt__kwabi_body(*mut c_void) -> i32
```

The generated function has exactly the `KwabiBodyFn` signature, so it is what
you pass to `kwabi_try`. The user's function stays ordinary Rust.

## Run

```sh
cd notes/runtime-skeleton/shim
make guard-test     PG=18     # guarded body: contained, backend alive
make guard-control  PG=18     # unguarded body: aborts (the control)
```

## The result

Identical on PostgreSQL 16.15, 17.11 and 18.6.

| body | build | result |
|---|---|---|
| `guarded_succeeds` | `#[guarded_body]` | `status=0` (KWABI_OK) |
| `guarded_reports_failure` | `#[guarded_body]` | `status=1` (KWABI_ERR_RAISED) |
| `guarded_maybe_panics` | `#[guarded_body]` | **`status=2` (KWABI_ERR_PANICKED), backend alive** |
| `raw_panicking_body` | hand-written `extern "C"` | **`signal 6: Abort trap`, postmaster restarts** |

The control matters. Without it, "the guarded body did not crash" would be
consistent with panics being contained anyway, and the macro doing nothing. The
control shows the same panic, unguarded, still takes down the server.

Also verified: a guarded panic inside a transaction leaves the transaction
usable (`SELECT 42` succeeds after), and the backend keeps serving.

## Why the trampoline must be generated

Writing it by hand is easy to get wrong, and the failure mode is a cluster-wide
abort rather than a compile error. Specifically:

- The `catch_unwind` must be **inside** the `extern "C"` frame. Outside it, the
  panic aborts at the boundary first (Rust ≥ 1.81) and the guard never runs.
- The `extern "C"` must be **on the trampoline**, not on the user's function.
- The argument must be reinterpreted from `*mut c_void` in exactly one place.

A macro is the only way to get a real `extern "C"` function with a stable name
while letting the body be ordinary Rust. A trait cannot express "this function
is also an `extern "C"` function pointer".

## What this does not fix

| case | outcome | why |
|---|---|---|
| panic in a body **not** built with the macro | still aborts | nothing catches it |
| panic during unwinding (a `Drop` that panics) | aborts | Rust's rule; nothing can intercept |
| `panic = "abort"` in the extension's profile | **build fails** | checked at compile time — see below |
| panic in the **runtime's** own code | contained by the runtime's `catch_unwind` | same std as the runtime |

### `panic = "abort"` is now a build error

This was the sharpest edge: the code compiled, the macro expanded, and the first
panic aborted. It is now caught at compile time, pointing at the developer's own
line:

```
error: kwabi: #[guarded_body] requires panic="unwind", but this crate is being
built with panic="abort". A panic in a guarded body cannot be caught under that
profile, so it would abort the PostgreSQL postmaster instead of returning
KWABI_ERR_PANICKED. Remove `panic = "abort"` from your profile, or set
`panic = "unwind"` explicitly.
  --> src/lib.rs:29:1
   |
29 | #[guarded_body]
   | ^^^^^^^^^^^^^^^
```

Hand-written trampolines are covered by an opt-in crate-root check:

```rust
kwabi::require_unwind!();
```

`build.rs` cannot do this — it runs before compilation and cannot see the panic
strategy. The full comparison of treatments is in
`notes/panic-abort-dx.md`.

## Files

| file | role |
|---|---|
| `vendor/kwabi/src/guarded.rs` | `contain_panic`, `IntoStatus`, the error slot |
| `vendor/kwabi/kwabi-macros/src/lib.rs` | the `#[guarded_body]` attribute macro |
| `runtime-skeleton/canary/guarded_bodies.rs` | the test bodies (guarded + raw control) |
| `runtime-skeleton/shim/guard-test.sql` | the containment test |
| `runtime-skeleton/shim/guard-control.sql` | the control (aborts) |

## Still open

- **No build-time enforcement of `panic = "unwind"`.** A `build.rs` in the SDK
  could emit `cargo:rustc-cfg` from `PROFILE`, but the panic strategy is not
  exposed to `build.rs` reliably. A CI check on the built artifact is more
  promising — an aborting profile can be detected by looking for the unwind
  tables.
- **The body cannot return a structured error.** *Closed since this was
  written* — see `abi-change-1-structured-errors.md`. The ABI grew the error
  channel and `publish_report` carries an extension's `KwabiReport` across.
- **`take_last_error` vs `last_error`.** Two accessors because the message is
  useful both ways; the consuming one makes double-reporting visible. Which is
  the default is still a judgement call.

## Reaching the runtime from inside a body

A body that needs to know something about the runtime — its version, its
capabilities — asks the SDK, which reads the table the extension stored at
`kwabi_ext_init` time. That only works if init has actually run, and there are
two ways to call a body:

| caller | calls `kwabi_ext_init`? |
|---|---|
| `kwabi_load_extension()` | yes — that is its whole job |
| `kwabi_try_symbol()` | yes, since the capability consumer needed it |

`kwabi_try_symbol` originally did not, and the guard bodies did not care
because none of them asked the SDK anything. The first body that did — the
capability consumer — got `None` and reported "the extension was not
initialised". The fix is in the shim; the lesson is that *a body's ability to
reach the runtime is a property of how it was called*, not only of the ABI.

`shim/capability-consumer.sql` exercises this, and its first check must stay
first: a psql session is one backend, so an earlier statement that initialised
the extension would mask a later one failing to. See
`capabilities-design.md` §7.5 for the negative control that caught exactly
that.
