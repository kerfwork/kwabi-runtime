//! kwabi runtime — per-version ABI adapter for PostgreSQL.
//!
//! # Two tables
//!
//! ```text
//!   extension.so ──calls──▶ KwabiV1   (stable, frozen, version-tagged)
//!                              ▲  published once at init
//!                              │
//!   kwabi_runtime.so ──────────┘
//!          │ binds at init
//!          ▼
//!   KwabiNative  (unstable: one build per PostgreSQL major version)
//! ```
//!
//! The runtime is compiled once per PostgreSQL major version (16, 17, 18).
//! Each build reaches that version's real internals, binds them into
//! `KwabiNative`, and publishes a single `KwabiV1` whose slots are the
//! runtime's own `extern "C"` wrappers.
//!
//! An extension compiled against `kwabi.h` links nothing from PostgreSQL. It
//! calls through `KwabiV1`, so the same binary loads on every supported major
//! version. That property — build once, run everywhere — is the whole point,
//! and `kwabi_runtime_init` below is the step that establishes it.
//!
//! # Scope of this skeleton
//!
//! Only the memory and error-firewall groups are wired. Every other slot in
//! the published table stays null, which is how this ABI reports "not
//! implemented yet". Wiring the remaining ~190 slots is the rest of the plan;
//! the mechanism does not change.

pub mod abi;
pub mod firewall;

use abi::KwabiV1;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::OnceLock;

// ========================================================================
// Native table — the runtime's private contract with one PostgreSQL major
// ========================================================================

/// PostgreSQL entry points this runtime build needs.
///
/// This is *not* the ABI. It changes whenever PostgreSQL does; `KwabiV1`
/// does not. Each major version gets its own `KwabiNative` populated by its
/// own build, which is why the runtime is compiled per major version and the
/// extension is not.
///
/// Every entry is `unsafe extern "C"` because these are raw PostgreSQL
/// symbols: the runtime must uphold PostgreSQL's contracts (memory context,
/// error handling, lock discipline) when it calls them.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct KwabiNative {
    /// PostgreSQL major version this table was bound against (16/17/18).
    pub pg_major: u32,
    pub palloc: Option<unsafe extern "C" fn(usize) -> *mut c_void>,
    pub palloc0: Option<unsafe extern "C" fn(usize) -> *mut c_void>,
    pub repalloc: Option<unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void>,
    pub pfree: Option<unsafe extern "C" fn(*mut c_void)>,
    pub memory_context_current: Option<unsafe extern "C" fn() -> *mut c_void>,
    pub memory_context_switch_to: Option<unsafe extern "C" fn(*mut c_void) -> *mut c_void>,
    pub memory_context_reset: Option<unsafe extern "C" fn(*mut c_void)>,
    pub memory_context_delete: Option<unsafe extern "C" fn(*mut c_void)>,
    pub error_message: Option<unsafe extern "C" fn() -> *const c_char>,
    pub error_code: Option<unsafe extern "C" fn() -> c_int>,
    pub error_clear: Option<unsafe extern "C" fn()>,
    /// Where the runtime sends its own log lines.
    pub log_line: Option<unsafe extern "C" fn(c_int, *const c_char)>,
}

// ========================================================================
// State
// ========================================================================

/// The bound native table, set once.
static NATIVE: OnceLock<KwabiNative> = OnceLock::new();

/// The published stable table. Its address is stable for the process
/// lifetime, which is what lets an extension cache the pointer.
static STABLE: OnceLock<KwabiV1> = OnceLock::new();

/// Runtime-owned error state. A failure raised *inside* the runtime must not
/// require raising a PostgreSQL error, because that would `longjmp` across
/// the ABI boundary — see the error-firewall principle.
static ERROR_CODE: AtomicI32 = AtomicI32::new(0);
static ERROR_SET: AtomicI32 = AtomicI32::new(0); // 0 = clear, 1 = set
static ERROR_MSG: OnceLock<&'static [u8]> = OnceLock::new();

fn native() -> Option<&'static KwabiNative> {
    NATIVE.get()
}

// ========================================================================
// Stable-ABI wrappers
// ========================================================================
//
// Each wrapper does exactly three things: check the runtime is initialised,
// reach the bound native pointer, and call it. Those checks are what make the
// table safe to hand to an extension built against a different minor version.

unsafe extern "C" fn rt_palloc(size: usize) -> *mut c_void {
    match native().and_then(|n| n.palloc) {
        Some(f) => f(size),
        None => ptr::null_mut(),
    }
}

unsafe extern "C" fn rt_palloc0(size: usize) -> *mut c_void {
    match native().and_then(|n| n.palloc0) {
        Some(f) => f(size),
        None => ptr::null_mut(),
    }
}

unsafe extern "C" fn rt_repalloc(p: *mut c_void, size: usize) -> *mut c_void {
    match native().and_then(|n| n.repalloc) {
        Some(f) => f(p, size),
        None => ptr::null_mut(),
    }
}

unsafe extern "C" fn rt_pfree(p: *mut c_void) {
    if let Some(f) = native().and_then(|n| n.pfree) {
        f(p);
    }
}

unsafe extern "C" fn rt_memory_context_current() -> *mut c_void {
    match native().and_then(|n| n.memory_context_current) {
        Some(f) => f(),
        None => ptr::null_mut(),
    }
}

unsafe extern "C" fn rt_memory_context_switch_to(ctx: *mut c_void) -> *mut c_void {
    match native().and_then(|n| n.memory_context_switch_to) {
        Some(f) => f(ctx),
        None => ptr::null_mut(),
    }
}

unsafe extern "C" fn rt_memory_context_reset(ctx: *mut c_void) {
    if let Some(f) = native().and_then(|n| n.memory_context_reset) {
        f(ctx);
    }
}

unsafe extern "C" fn rt_memory_context_delete(ctx: *mut c_void) {
    if let Some(f) = native().and_then(|n| n.memory_context_delete) {
        f(ctx);
    }
}

unsafe extern "C" fn rt_error_message() -> *const c_char {
    // Runtime-owned state wins: an error raised inside the runtime is more
    // recent than anything PostgreSQL last reported.
    if ERROR_SET.load(Ordering::Acquire) == 1 {
        if let Some(msg) = ERROR_MSG.get() {
            return msg.as_ptr() as *const c_char;
        }
    }
    match native().and_then(|n| n.error_message) {
        Some(f) => f(),
        None => ptr::null(),
    }
}

unsafe extern "C" fn rt_error_code() -> c_int {
    if ERROR_SET.load(Ordering::Acquire) == 1 {
        return ERROR_CODE.load(Ordering::Acquire);
    }
    match native().and_then(|n| n.error_code) {
        Some(f) => f(),
        None => 0,
    }
}

unsafe extern "C" fn rt_error_clear() {
    ERROR_SET.store(0, Ordering::Release);
    ERROR_CODE.store(0, Ordering::Release);
    if let Some(f) = native().and_then(|n| n.error_clear) {
        f();
    }
}

unsafe extern "C" fn rt_log_line(level: c_int, msg: *const c_char) {
    if let Some(f) = native().and_then(|n| n.log_line) {
        f(level, msg);
    }
}

// ========================================================================
// Lifecycle
// ========================================================================

/// Build the stable table and wire the groups this skeleton implements.
/// What a table guarantees, DERIVED from what is actually wired in it.
///
/// A PURE FUNCTION OF THE TABLE, and that matters. The obvious version read
/// the runtime's own `STABLE` — which is the wrong table: the shim installs
/// `try_body`, `error_get` and the memory accessors onto a *copy* afterwards,
/// so the runtime's table has those slots NULL and the derived answer was
/// wrong (it claimed CORE only, on a runtime that supports everything).
///
/// Taking the table as an argument keeps ONE implementation and lets each
/// layer ask about the table it actually published. The shim calls this with
/// its final table; `rt_capabilities` calls it with `STABLE` for the case
/// where there is no shim.
///
/// Why derive rather than declare: a declared bitset drifts the moment someone
/// adds a slot and forgets the bit. A capability bit that lies is worse than
/// no bit, because an extension will rely on it and fail undiagnosably.
///
/// # Safety
///
/// `t` must be null or point to a valid `KwabiV1`.
#[allow(clippy::missing_safety_doc)]
pub unsafe fn capabilities_of(t: *const KwabiV1, pg_major: u32) -> u64 {
    if t.is_null() {
        return 0;
    }
    let t = &*t;
    let mut caps = 0u64;

    // CORE: the slots whose absence would mean this is not a runtime.
    if t.palloc.is_some()
        && t.pfree.is_some()
        && t.memory_context_current.is_some()
        && t.error_message.is_some()
    {
        caps |= abi::KWABI_CAP_CORE;
    }

    // STRUCTURED_ERRORS: two conditions, and the second is the one a naive
    // version misses.
    //
    //  1. the struct we compiled against can carry the extended fields, so a
    //     future build with a smaller KwabiError cannot claim the bit;
    //  2. there is a slot to DELIVER one through.
    //
    // Condition 2 was found by the negative test: a table with nothing wired
    // still claimed this bit, because the struct's size is a property of the
    // build rather than of the table. But a runtime with no `error_get` and no
    // `try_body` has no way to hand a structured error to anyone, so claiming
    // it is a lie — and a lie an extension would rely on.
    let can_deliver = t.error_get.is_some() || t.try_body.is_some();
    if can_deliver
        && std::mem::size_of::<abi::KwabiError>() >= abi::KWABI_ERR_STRUCTURED_MIN
    {
        caps |= abi::KWABI_CAP_STRUCTURED_ERRORS;
    }

    // ERROR_FIREWALL: the catching direction is present.
    if t.try_body.is_some() && t.error_get.is_some() {
        caps |= abi::KWABI_CAP_ERROR_FIREWALL;
    }

    // MEMORY_INTROSPECTION: both accessors present. These are shim-installed,
    // which is precisely why this function must read the shim's table.
    if t.memory_chunk_context.is_some() && t.current_memory_context.is_some() {
        caps |= abi::KWABI_CAP_MEMORY_INTROSPECTION;
    }

    // ATOMIC_BODY: the firewall exists AND the PostgreSQL major is one the CI
    // matrix has MEASURED. This is the bit that must never be guessed — it
    // promises that a failed body leaves no partial work, which is an
    // empirical claim about subtransaction behaviour, not a structural one.
    // A major we have not exercised does not get the bit.
    let measured_majors = [16u32, 17, 18];
    if caps & abi::KWABI_CAP_ERROR_FIREWALL != 0 && measured_majors.contains(&pg_major) {
        caps |= abi::KWABI_CAP_ATOMIC_BODY;
    }

    caps
}

/// The ABI slot: capabilities of the table this runtime published.
unsafe extern "C" fn rt_capabilities() -> u64 {
    let t = STABLE.get_or_init(build_table);
    capabilities_of(t as *const KwabiV1, pg_major())
}

fn build_table() -> KwabiV1 {
    let mut t = KwabiV1::default();
    t.palloc = Some(rt_palloc);
    t.palloc0 = Some(rt_palloc0);
    t.repalloc = Some(rt_repalloc);
    t.pfree = Some(rt_pfree);
    t.memory_context_current = Some(rt_memory_context_current);
    t.memory_context_switch_to = Some(rt_memory_context_switch_to);
    t.memory_context_reset = Some(rt_memory_context_reset);
    t.memory_context_delete = Some(rt_memory_context_delete);
    t.error_message = Some(rt_error_message);
    t.error_code = Some(rt_error_code);
    t.error_clear = Some(rt_error_clear);
    t.ereport = Some(rt_ereport);
    t.elog = Some(rt_elog);

    // `memory_chunk_context`, `current_memory_context` and `raise_error` are
    // deliberately left null here. They are installed by the per-version C
    // shim, because `raise_error` raises a real PostgreSQL ERROR and must not
    // be entered from a Rust frame. A runtime running without its shim simply
    // reports those services as absent, which is the correct signal.
    t
}

/// `ereport` through the ABI takes a pre-formatted message.
///
/// PostgreSQL's own `ereport` is a variadic macro, not a function, so it
/// cannot be forwarded across a stable ABI. Formatting happens on the
/// extension side; the runtime only transports the result. The header's third
/// parameter is the varargs slot and is ignored here.
unsafe extern "C" fn rt_ereport(code: c_int, msg: *const c_char, _varargs: *const c_char) {
    rt_log_line(code, msg);
}

/// `elog` through the ABI, same shape as `rt_ereport`.
unsafe extern "C" fn rt_elog(level: c_int, msg: *const c_char, _varargs: *const c_char) {
    rt_log_line(level, msg);
}

/// Initialise the runtime against a specific PostgreSQL build.
///
/// In production this is called from the runtime's `_PG_init`, which fills in
/// a `KwabiNative` from real PostgreSQL symbols. The test harness calls it
/// with stub functions to prove the handoff.
///
/// Returns the published stable table. Safe to call more than once and from
/// more than one thread: `NATIVE` and `STABLE` are both `OnceLock`, so the
/// first caller wins and every caller gets the same table address. That
/// address stability is load-bearing — an extension caches the pointer.
///
/// # Safety
///
/// `native` must point to a valid `KwabiNative` that outlives the process.
#[no_mangle]
/// Capabilities of a caller-supplied table. Exported for the shim, which owns
/// the final table (it installs `try_body`, `error_get` and the memory
/// accessors onto a copy of the runtime's). Asking the runtime's own table
/// would report those slots as absent and understate what the ABI supports.
///
/// # Safety
///
/// `table` must be null or point to a valid `KwabiV1`.
#[no_mangle]
pub unsafe extern "C" fn kwabi_capabilities_of(table: *const KwabiV1, pg_major: u32) -> u64 {
    capabilities_of(table, pg_major)
}

/// Capabilities of a caller-supplied table. Exported for the shim, which owns
/// the final table (it installs `try_body`, `error_get` and the memory
/// accessors onto a copy of the runtime's). Asking the runtime's own table
/// would report those slots as absent and understate what the ABI supports.
///
/// # Safety
///
/// `table` must be null or point to a valid `KwabiV1`.
#[no_mangle]
pub unsafe extern "C" fn kwabi_runtime_init(native: *const KwabiNative) -> *const KwabiV1 {
    if native.is_null() {
        return ptr::null();
    }
    let n = *native;

    // Losing this race is fine: it means someone else bound the same build.
    let _ = NATIVE.set(n);

    // get_or_init runs build_table exactly once even under contention, so
    // concurrent callers cannot observe a half-published table.
    STABLE.get_or_init(build_table) as *const KwabiV1
}

/// Fetch the published table without re-initialising.
///
/// This is the entry point an extension loader calls.
#[no_mangle]
pub extern "C" fn kwabi_get_api() -> *const KwabiV1 {
    STABLE.get().map(|t| t as *const KwabiV1).unwrap_or(ptr::null())
}

/// Record an error in the runtime's own state.
///
/// This is the direction of the error firewall that never crosses into
/// PostgreSQL: the runtime can fail an extension call without raising a
/// PostgreSQL error, so no `longjmp` unwinds through extension frames.
///
/// # Safety
///
/// `msg` must be a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn kwabi_raise(code: c_int, msg: *const c_char) -> bool {
    if msg.is_null() {
        return false;
    }
    let bytes = std::ffi::CStr::from_ptr(msg).to_bytes_with_nul();
    // Only the first raise is retained; a real runtime owns a per-backend
    // error buffer instead of a OnceLock.
    let _ = ERROR_MSG.set(Box::leak(bytes.to_vec().into_boxed_slice()));
    ERROR_CODE.store(code, Ordering::Release);
    ERROR_SET.store(1, Ordering::Release);
    true
}

/// The published stable table, for Rust extension code linking this crate.
pub fn api() -> Option<&'static KwabiV1> {
    STABLE.get()
}

// ========================================================================
// Error firewall: the catching direction
// ========================================================================

/// Run a guarded body, containing any panic, and report the outcome.
///
/// This is the Rust half of `kwabi_try`. The other half is in the C shim,
/// because `PG_TRY` cannot live here: the guarded body is reached from Rust, so
/// a `longjmp` out of it would cross Rust frames — the one thing the firewall
/// forbids. The shim installs the `PG_TRY` and the subtransaction; this
/// function makes sure nothing unwinds.
///
/// Call order, shim → runtime:
///
/// ```text
///   shim: try_body(body, arg, out)
///     ├─ begin internal subtransaction
///     ├─ PG_TRY
///     │    └─ runtime: guard_body(body, arg)   <-- this function
///     │         └─ catch_unwind(body(arg))
///     ├─ PG_CATCH  → copy error, roll back, fill `out`
///     └─ restore, return status
/// ```
///
/// Note what is *not* here: no PostgreSQL call, and therefore no way for a
/// `longjmp` to be raised while this frame is on the stack. `guard_body` calls
/// only the body, which the contract requires not to raise.
///
/// # Safety
///
/// `body` must be a valid function pointer; `arg` must be valid for it; `out`
/// must be null or point to a writable `KwabiError`.
#[no_mangle]
pub unsafe extern "C" fn kwabi_try_body(
    body: Option<abi::KwabiBodyFn>,
    arg: *mut c_void,
    out: *mut abi::KwabiError,
) -> c_int {
    // Validate before doing anything that could fail.
    let rc = firewall::check_try_args(out);
    if rc != abi::KWABI_OK {
        return rc;
    }

    let body = match body {
        Some(f) => f,
        None => {
            firewall::set_last_error(
                abi::KWABI_ERR_BAD_ARG,
                0,
                "kwabi: try_body was given a null body",
            );
            *out = firewall::last_error();
            return abi::KWABI_ERR_BAD_ARG;
        }
    };

    let status = firewall::guard_body(body, arg, out);

    // PRECEDENCE, and it matters.
    //
    // The trampoline may already have written the extension's own structured
    // error into `out` (see publish_report in the SDK). If it did, that error
    // is strictly better than anything the runtime can produce: the runtime
    // saw only a status, while the extension knows its SQLSTATE, detail, hint
    // and object names.
    //
    // So the runtime writes only when the body did NOT report. Writing
    // unconditionally clobbers the extension's error with an empty buffer —
    // which is exactly what happened before this check existed, and produced
    // `status=1 msg=""` for a body that had reported a full structured error.
    //
    // The test for "did the body report" is a non-OK status in `out` that the
    // runtime did not put there. Since `guard_body` only sets the runtime's
    // buffer and never `out` directly, a populated `out` means the extension
    // wrote it.
    let body_reported = status != abi::KWABI_OK
        && !out.is_null()
        && (*out).status == status
        && (*out).message[0] != 0;

    if !body_reported {
        if status == abi::KWABI_OK {
            // Clear it, so a caller that ignores the return value cannot read
            // a stale message and believe it.
            let cleared = abi::KwabiError::default();
            firewall::write_error_to(out, &cleared);
        } else {
            let err = firewall::last_error();
            firewall::write_error_to(out, &err);
        }
    }

    status
}

/// Fetch the runtime's last error without running a body.
///
/// Lets a caller inspect the error after a `try_body` returned, or after any
/// other slot failed.
///
/// # Safety
///
/// `out` must point to a writable `KwabiError`.
#[no_mangle]
pub unsafe extern "C" fn kwabi_error_get(out: *mut abi::KwabiError) {
    if out.is_null() {
        return;
    }
    let err = firewall::last_error();
    firewall::write_error_to(out, &err);
}

/// Overwrite the runtime's error buffer from a caller-built `KwabiError`.
///
/// The mirror of `kwabi_error_get`. It exists for the shim's fmgr slots: they
/// capture a PostgreSQL error in C (where `PG_CATCH` lives), build a
/// `KwabiError` with the header's own helpers, and need it to land in the same
/// buffer `error_get` reads. Without this the shim could catch an error and the
/// extension could never read it.
///
/// A full-struct copy, deliberately: the shim always builds its own complete
/// `KwabiError`, so there is no size negotiation here. Size negotiation happens
/// on the *read* side, in `kwabi_error_get`, where the caller's struct may be
/// an older, smaller one.
///
/// # Safety
///
/// `e` must point to a readable `KwabiError`.
#[no_mangle]
pub unsafe extern "C" fn kwabi_error_set(e: *const abi::KwabiError) {
    if e.is_null() {
        return;
    }
    firewall::set_last_error_from(&*e);
}

/// Whether the runtime has been initialised.
pub fn is_initialised() -> bool {
    STABLE.get().is_some()
}

/// The PostgreSQL major version this build bound against.
pub fn pg_major() -> u32 {
    native().map(|n| n.pg_major).unwrap_or(0)
}
