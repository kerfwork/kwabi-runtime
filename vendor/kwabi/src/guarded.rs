//! Guarded bodies: making "must not panic" true by construction.
//!
//! # The problem
//!
//! `kwabi_try` runs a body inside the C shim's `PG_TRY`. The contract says the
//! body must not panic, but nothing enforced it. A body that panicked aborted
//! the **whole postmaster** — measured, not theoretical:
//!
//! ```text
//! fatal runtime error: Rust cannot catch foreign exceptions, aborting
//! LOG:  client process was terminated by signal 6: Abort trap: 6
//! LOG:  terminating any other active server processes
//! ```
//!
//! The runtime's `catch_unwind` cannot help: the extension is a separately
//! built artifact with its own copy of Rust std, so its panic is a *foreign
//! exception* to the runtime's std and `catch_unwind` refuses it.
//!
//! # The fix
//!
//! Catch the panic **in the extension's own crate**, where the std is the same
//! one that raised it. A panic caught by its own std is an ordinary `Err`; it
//! never reaches a boundary, so it never aborts.
//!
//! Writing that trampoline by hand is easy to get wrong, and the failure mode
//! is a cluster-wide abort rather than a compile error. So the SDK generates it:
//!
//! ```rust
//! use kwabi::guarded_body;
//!
//! struct Arg { n: i32 }
//!
//! #[guarded_body]
//! fn attempt(arg: &mut Arg) -> Result<(), String> {
//!     if arg.n < 0 { panic!("negative"); }   // contained -> KWABI_ERR_PANICKED
//!     Ok(())
//! }
//! // generates: `extern "C" fn attempt__kwabi_body(*mut c_void) -> i32`
//! ```
//!
//! The generated function has exactly the signature `KwabiBodyFn` expects, so
//! it is what gets passed to `kwabi_try`. Hand-written bodies remain possible —
//! the ABI cannot forbid them — but the supported path cannot abort the server.
//!
//! # What this does not fix
//!
//! - A panic in a body **not** built with this macro. Still aborts.
//! - A panic during unwinding (a `Drop` that panics). Rust aborts; that is
//!   Rust's rule and nothing here can intercept it.
//! - `panic = "abort"` in the extension's profile. Then there is nothing to
//!   catch. Builds must use the default `panic = "unwind"`.

use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

/// Status values, mirroring `KwabiStatus` in `kwabi.h`.
pub const KWABI_OK: i32 = 0;
pub const KWABI_ERR_RAISED: i32 = 1;
pub const KWABI_ERR_PANICKED: i32 = 2;
pub const KWABI_ERR_BODY_RAISED: i32 = 3;
pub const KWABI_ERR_BAD_ARG: i32 = 4;

/// Message from the most recent contained panic or reported failure.
///
/// Owned by the extension, not the runtime: the runtime cannot see this
/// extension's panics at all, which is the whole point.
static mut LAST_ERROR: Option<String> = None;

/// Take the message from the most recent contained failure, if any.
///
/// Moves it out, so a second call returns `None`. That makes double-reporting
/// visible instead of silently handing the same message to two callers.
pub fn take_last_error() -> Option<String> {
    // Single-threaded per backend: PostgreSQL does not run extension entry
    // points concurrently within a backend.
    unsafe {
        let slot = &mut *std::ptr::addr_of_mut!(LAST_ERROR);
        slot.take()
    }
}

/// Borrow the last message without consuming it.
pub fn last_error() -> Option<&'static str> {
    unsafe {
        let slot = &*std::ptr::addr_of!(LAST_ERROR);
        slot.as_deref()
    }
}

/// Record a message for the extension's own diagnostics.
///
/// # Safety
///
/// Single-threaded per backend.
pub unsafe fn set_last_error(msg: String) {
    let slot = &mut *std::ptr::addr_of_mut!(LAST_ERROR);
    *slot = Some(msg);
}

/// Run a closure, containing any panic it raises.
///
/// The building block the macro expands to. Exposed because a hand-written
/// trampoline may need it, and because it is the one place the catch happens —
/// a second implementation would be a second thing to get wrong.
pub fn contain_panic<F, T>(f: F) -> Result<T, String>
where
    F: FnOnce() -> T,
{
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(v) => Ok(v),
        Err(payload) => Err(panic_message(payload)),
    }
}

/// Extract a readable message from a panic payload.
///
/// `panic!("literal")`, `panic!("{}", x)` and `panic_any` produce different
/// payload types. All are handled so the operator sees the cause rather than
/// "unknown panic".
fn panic_message(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&'static str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "panic with a non-string payload".to_string()
    }
}

/// A raw body pointer, as `kwabi_try` expects it.
pub type RawBodyFn = unsafe extern "C" fn(*mut c_void) -> i32;

/// A guarded body, as the ABI defines it: `KwabiBodyFn` in `kwabi.h`.
///
/// The second parameter is the caller's error channel, and it is why a body
/// can report a structured error rather than only a status. The trampoline
/// `#[guarded_body]` generates has exactly this signature.
pub type KwabiBodyFn = unsafe extern "C" fn(*mut c_void, *mut KwabiErrorAbi) -> i32;

/// Run a generated body, containing panics.
///
/// The one unsafe step: the trampoline receives a `*mut c_void` and the body
/// wants `&mut A`. The pointer must point to a live `A` that the caller owns
/// for the duration of the call — which is exactly what `kwabi_try`'s `arg`
/// parameter is.
///
/// The body borrows rather than takes ownership, so the caller's value is
/// still valid after the call returns. That matters because the subtransaction
/// rollback can happen around this call, and the caller may want to inspect
/// what it passed in.
///
/// # Safety
///
/// `arg` must be null or point to a valid `A`.
pub unsafe fn run_guarded<A, R, F>(arg: *mut c_void, f: F) -> Result<R, String>
where
    F: FnOnce(&mut A) -> R,
{
    if arg.is_null() {
        return Err("kwabi: guarded body received a null argument".to_string());
    }
    let a = &mut *(arg as *mut A);
    contain_panic(|| f(a))
}

/// Map a guarded outcome onto a `KwabiStatus`.
///
/// Kept out of the macro so the expansion stays short enough to read.
pub fn status_from_outcome<R: IntoStatus>(outcome: Result<R, String>) -> i32 {
    match outcome {
        Ok(v) => v.into_status(),
        Err(msg) => {
            unsafe { set_last_error(msg) };
            KWABI_ERR_PANICKED
        }
    }
}

/// Write the extension's structured error into the caller's `KwabiError`.
///
/// This is the piece that closes the channel. The runtime cannot see an
/// extension's errors — they live in this crate — so the trampoline has to
/// copy them across itself, into the same `out` the caller passed to
/// `kwabi_try`.
///
/// Size-guarded, exactly as the runtime's writer is: the caller's struct may
/// come from an older, smaller header.
///
/// # Safety
///
/// `out` must be null or point to a writable `KwabiError` whose `size` is
/// accurate.
pub unsafe fn publish_report(out: *mut KwabiErrorAbi, status: i32) {
    if out.is_null() || status == KWABI_OK {
        return;
    }
    let report = match take_report() {
        Some(r) => r,
        None => return,   // not a structured report; leave the runtime's message
    };

    // The caller's KwabiError starts with the same four fields the runtime's
    // does, so we can reach it without depending on the full layout.
    let err = out;
    let caller_size = (*err).size as usize;
    let core_end = std::mem::offset_of!(KwabiErrorAbi, message) + 32;
    if caller_size < core_end {
        return;
    }

    (*err).sqlerrcode = report.sqlstate_code();
    (*err).status = status;
    store_c_buf(&mut (*err).message, &report.message);

    macro_rules! guarded {
        ($field:ident, $value:expr) => {
            if caller_size
                >= std::mem::offset_of!(KwabiErrorAbi, $field)
                    + std::mem::size_of_val(&(*err).$field)
            {
                store_c_buf(&mut (*err).$field, $value);
            }
        };
    }
    guarded!(detail, &report.detail);
    guarded!(hint, &report.hint);
    guarded!(schema_name, &report.schema_name);
    guarded!(table_name, &report.table_name);
    guarded!(column_name, &report.column_name);
    guarded!(datatype_name, &report.datatype_name);
    guarded!(constraint_name, &report.constraint_name);
}

/// Copy `s` into a fixed C buffer, truncating and always NUL-terminating.
///
/// The SDK's own copy: the runtime crate has an equivalent, but an extension
/// links the SDK, not the runtime, so it cannot borrow that one.
fn store_c_buf<const N: usize>(dst: &mut [c_char; N], s: &str) {
    let bytes = s.as_bytes();
    let n = bytes.len().min(N - 1);
    for (i, b) in bytes.iter().take(n).enumerate() {
        dst[i] = *b as c_char;
    }
    dst[n] = 0;
}

// ========================================================================
// ABI error layout (mirrors kwabi.h)
//
// The SDK is what an extension links, so it must own the types the extension
// touches. The runtime crate has its own generated mirror of the same struct;
// both are checked against kwabi.h by their respective test suites.
// ========================================================================

/// Field sizes. Must match the KWABI_ERR*_MAX defines in kwabi.h.
pub const KWABI_ERRMSG_MAX: usize = 512;
pub const KWABI_ERRDETAIL_MAX: usize = 512;
pub const KWABI_ERRHINT_MAX: usize = 384;
pub const KWABI_ERRNAME_MAX: usize = 128;

/// The error channel, as the extension sees it.
///
/// `size` is first and must stay first: it is what lets a writer built against
/// a newer header refuse to write past an older caller's smaller struct.
#[repr(C)]
pub struct KwabiErrorAbi {
    pub size: u32,
    pub sqlerrcode: i32,
    pub status: i32,
    pub message: [c_char; KWABI_ERRMSG_MAX],
    pub detail: [c_char; KWABI_ERRDETAIL_MAX],
    pub hint: [c_char; KWABI_ERRHINT_MAX],
    pub schema_name: [c_char; KWABI_ERRNAME_MAX],
    pub table_name: [c_char; KWABI_ERRNAME_MAX],
    pub column_name: [c_char; KWABI_ERRNAME_MAX],
    pub datatype_name: [c_char; KWABI_ERRNAME_MAX],
    pub constraint_name: [c_char; KWABI_ERRNAME_MAX],
}

// ========================================================================
// Capabilities (mirrors kwabi.h)
// ========================================================================

pub const KWABI_CAP_CORE: u64 = 1 << 0;
pub const KWABI_CAP_STRUCTURED_ERRORS: u64 = 1 << 1;
pub const KWABI_CAP_ERROR_FIREWALL: u64 = 1 << 2;
pub const KWABI_CAP_MEMORY_INTROSPECTION: u64 = 1 << 3;
pub const KWABI_CAP_ATOMIC_BODY: u64 = 1 << 4;

/// What a runtime GUARANTEES, as distinct from which slots exist.
///
/// An extension asks this instead of testing a slot when it needs a
/// *guarantee* rather than merely a function. The distinction that matters:
/// a non-NULL `try_body` says the slot exists; only `ATOMIC_BODY` says a
/// failed body's writes are rolled back.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Capabilities(pub u64);

impl Capabilities {
    pub fn has(self, bit: u64) -> bool {
        self.0 & bit != 0
    }

    /// Can a failed body's partial work be trusted to be undone?
    ///
    /// The question an extension that WRITES must ask before relying on the
    /// firewall. `ERROR_FIREWALL` alone is not enough: it says the error is
    /// contained, not that the writes are.
    pub fn atomic_bodies(self) -> bool {
        self.has(KWABI_CAP_ATOMIC_BODY)
    }

    /// Will the extended error fields actually be filled?
    pub fn structured_errors(self) -> bool {
        self.has(KWABI_CAP_STRUCTURED_ERRORS)
    }

    /// Names of the set bits, for logging and error messages.
    pub fn names(self) -> String {
        let table = [
            (KWABI_CAP_CORE, "CORE"),
            (KWABI_CAP_STRUCTURED_ERRORS, "STRUCTURED_ERRORS"),
            (KWABI_CAP_ERROR_FIREWALL, "ERROR_FIREWALL"),
            (KWABI_CAP_MEMORY_INTROSPECTION, "MEMORY_INTROSPECTION"),
            (KWABI_CAP_ATOMIC_BODY, "ATOMIC_BODY"),
        ];
        let set: Vec<&str> = table
            .iter()
            .filter(|(b, _)| self.has(*b))
            .map(|(_, n)| *n)
            .collect();
        if set.is_empty() {
            "(none)".to_string()
        } else {
            set.join("|")
        }
    }
}

/// The bootstrap rule, as code rather than as documentation.
///
/// A runtime older than the `capabilities` slot has no way to answer, so the
/// slot is NULL. That must mean "no capability query — fall back to testing
/// slots directly", NOT "nothing works". Getting this wrong makes an
/// extension refuse to load against a runtime that is merely old, which is
/// the opposite of what this ABI exists for.
///
/// `slot` is the raw `api->capabilities` field: `None` when absent.
///
/// ```rust
/// # use kwabi::guarded::{capabilities_from_slot, KWABI_CAP_CORE};
/// // An old runtime: no slot. Assume CORE and carry on.
/// let caps = capabilities_from_slot(None, 16);
/// assert!(caps.has(KWABI_CAP_CORE));
/// ```
pub fn capabilities_from_slot(
    slot: Option<unsafe extern "C" fn() -> u64>,
    pg_major: u32,
) -> Capabilities {
    match slot {
        Some(f) => Capabilities(unsafe { f() }),
        None => {
            // No query available. CORE is the honest floor: it is what any
            // runtime that got this far must support. Anything stronger is
            // unverifiable, so it is not claimed.
            let _ = pg_major; // kept for symmetry with the runtime's answer
            Capabilities(KWABI_CAP_CORE)
        }
    }
}

/// A structured error an extension can report.
///
/// This is what makes the DX payoff real: instead of a `String` that gets
/// flattened into one line, an extension names the SQLSTATE, the message, and
/// the optional detail/hint/object fields. A client can then handle the error
/// programmatically — match on the SQLSTATE, highlight the offending column —
/// rather than parsing prose.
///
/// ```rust
/// # use kwabi::guarded::KwabiReport;
/// fn report() -> Result<(), KwabiReport> {
///     Err(KwabiReport::new("22023", "value out of range")
///         .detail("Allowed range is 1 to 100.")
///         .hint("Check the extension's configuration.")
///         .column("amount"))
/// }
/// ```
#[derive(Debug, Clone, Default)]
pub struct KwabiReport {
    /// A PostgreSQL SQLSTATE string, e.g. "22023". Parsed to the integer form
    /// when written. Invalid or empty means class 50 (application-defined).
    pub sqlstate: String,
    pub message: String,
    pub detail: String,
    pub hint: String,
    pub schema_name: String,
    pub table_name: String,
    pub column_name: String,
    pub datatype_name: String,
    pub constraint_name: String,
}

impl KwabiReport {
    pub fn new(sqlstate: &str, message: &str) -> Self {
        KwabiReport {
            sqlstate: sqlstate.to_string(),
            message: message.to_string(),
            ..Default::default()
        }
    }

    pub fn detail(mut self, s: &str) -> Self { self.detail = s.to_string(); self }
    pub fn hint(mut self, s: &str) -> Self { self.hint = s.to_string(); self }
    pub fn schema(mut self, s: &str) -> Self { self.schema_name = s.to_string(); self }
    pub fn table(mut self, s: &str) -> Self { self.table_name = s.to_string(); self }
    pub fn column(mut self, s: &str) -> Self { self.column_name = s.to_string(); self }
    pub fn datatype(mut self, s: &str) -> Self { self.datatype_name = s.to_string(); self }
    pub fn constraint(mut self, s: &str) -> Self { self.constraint_name = s.to_string(); self }

    /// Convert a 5-character SQLSTATE into PostgreSQL's integer encoding.
    ///
    /// PostgreSQL packs a SQLSTATE as five base-36-ish digits: the first two
    /// characters are the class, the last three the subcode, each mapped from
    /// the character set `0-9A-Z`. Getting this wrong yields an error code the
    /// client cannot match on, so it is done in one place.
    pub fn sqlstate_code(&self) -> i32 {
        parse_sqlstate(&self.sqlstate).unwrap_or(KWABI_SQLSTATE_APPLICATION)
    }
}

/// SQLSTATE class 50: application-defined. Used when an extension supplies no
/// code, so its errors stay distinguishable from core ones.
pub const KWABI_SQLSTATE_APPLICATION: i32 = 0x50000;

/// Parse a 5-character SQLSTATE into PostgreSQL's integer form.
///
/// Each character maps to 0-35 (`0-9` then `A-Z`); the value is
/// `(d1*36 + d2) * 36^3 + (d3*36 + d4) * 36 + d5` — i.e. the first two
/// characters form the class, the last three the subcode.
pub fn parse_sqlstate(s: &str) -> Option<i32> {
    let b = s.as_bytes();
    if b.len() != 5 {
        return None;
    }
    fn digit(c: u8) -> Option<i32> {
        match c {
            b'0'..=b'9' => Some((c - b'0') as i32),
            b'A'..=b'Z' => Some((c - b'A') as i32 + 10),
            b'a'..=b'z' => Some((c - b'a') as i32 + 10),
            _ => None,
        }
    }
    let d: Vec<i32> = b.iter().filter_map(|c| digit(*c)).collect();
    if d.len() != 5 {
        return None;
    }
    // (class * 36^3) + subcode, matching PostgreSQL's MAKE_SQLSTATE.
    let class = d[0] * 36 + d[1];
    let subcode = (d[2] * 36 + d[3]) * 36 + d[4];
    Some((class << 18) | subcode)
}

/// What a guarded body may return.
///
/// `()` means success. `Result<(), E>` means success or a reported failure,
/// where `E: Debug` is flattened into a message. `E` is deliberately not
/// constrained to a kwabi error type: the ABI has no structured error channel
/// yet, so an extension's own error type is the natural thing to use and is
/// stringified on the way out.
pub trait IntoStatus {
    fn into_status(self) -> i32;
}

impl IntoStatus for () {
    fn into_status(self) -> i32 {
        KWABI_OK
    }
}

/// An extension's own error type, as a guarded body may return it.
///
/// Implement this for your error enum to get its message into the caller's
/// `KwabiError`. Implement `IntoReport` instead if you want the structured
/// fields — see `KwabiReport`.
///
/// Why a trait and not a blanket `impl<E: Debug>`: `KwabiReport` is `Debug`,
/// so a blanket impl would overlap with its own. A trait keeps the two cases
/// distinct and lets the compiler say which one a type uses.
pub trait IntoError {
    /// The primary message.
    fn message(&self) -> String;
}

impl IntoStatus for Result<(), Box<dyn IntoError>> {
    fn into_status(self) -> i32 {
        match self {
            Ok(()) => KWABI_OK,
            Err(e) => {
                unsafe { set_last_error(e.message()) };
                KWABI_ERR_RAISED
            }
        }
    }
}

/// A plain `String` error: the common case, flattened to the message field.
impl IntoStatus for Result<(), String> {
    fn into_status(self) -> i32 {
        match self {
            Ok(()) => KWABI_OK,
            Err(e) => {
                unsafe { set_last_error(e) };
                KWABI_ERR_RAISED
            }
        }
    }
}

/// `&'static str` for the same reason.
impl IntoStatus for Result<(), &'static str> {
    fn into_status(self) -> i32 {
        match self {
            Ok(()) => KWABI_OK,
            Err(e) => {
                unsafe { set_last_error(e.to_string()) };
                KWABI_ERR_RAISED
            }
        }
    }
}

/// An extension error that carries structured fields.
///
/// Implemented for `KwabiReport`. Implement it for your own type if you want
/// to control the mapping without converting to `KwabiReport` first.
pub trait IntoReport {
    fn report(&self) -> KwabiReport;
}

/// A structured error, reported with its fields intact.
///
/// This is the DX win over `Result<(), String>`: the extension's own error type
/// reaches the caller as SQLSTATE + message + detail + hint + object names,
/// rather than as one flattened line.
impl IntoStatus for Result<(), KwabiReport> {
    fn into_status(self) -> i32 {
        match self {
            Ok(()) => KWABI_OK,
            Err(r) => {
                unsafe { store_report(&r) };
                KWABI_ERR_RAISED
            }
        }
    }
}

impl IntoError for KwabiReport {
    fn message(&self) -> String {
        self.message.clone()
    }
}

impl IntoReport for KwabiReport {
    fn report(&self) -> KwabiReport {
        self.clone()
    }
}

/// The extension's own structured error, kept until the caller reads it.
///
/// Separate from the runtime's buffer: the runtime cannot see an extension's
/// errors, so the extension has to carry them across itself. The trampoline
/// copies this into the caller's `KwabiError` before returning.
static mut LAST_REPORT: Option<KwabiReport> = None;

unsafe fn store_report(r: &KwabiReport) {
    let slot = &mut *std::ptr::addr_of_mut!(LAST_REPORT);
    *slot = Some(r.clone());
}

/// Take the extension's most recent structured error, if any.
pub fn take_report() -> Option<KwabiReport> {
    unsafe {
        let slot = &mut *std::ptr::addr_of_mut!(LAST_REPORT);
        slot.take()
    }
}

#[cfg(test)]
mod capability_tests {
    use super::*;

    /// The bootstrap rule, as a test rather than a comment.
    ///
    /// A NULL slot must yield CORE-only. The failure this guards against is
    /// reading it as zero, which would make an extension refuse to load against
    /// a runtime that is merely older than the slot — the opposite of what this
    /// ABI is for.
    #[test]
    fn null_slot_yields_core_not_zero() {
        let caps = capabilities_from_slot(None, 16);
        assert!(
            caps.has(KWABI_CAP_CORE),
            "a runtime older than the slot must still be usable"
        );
        assert_ne!(caps.0, 0, "NULL must not mean 'nothing works'");
        assert_eq!(
            caps.0, KWABI_CAP_CORE,
            "nothing stronger than CORE is verifiable without a slot"
        );
    }

    /// A present slot is asked, and its answer is taken as-is.
    ///
    /// The point of the pairing: the two paths must be distinguishable. A test
    /// that only ever saw `None` would pass against an implementation that
    /// ignored the slot entirely.
    #[test]
    fn present_slot_is_asked() {
        unsafe extern "C" fn claims_firewall() -> u64 {
            KWABI_CAP_CORE | KWABI_CAP_ERROR_FIREWALL
        }
        let caps = capabilities_from_slot(Some(claims_firewall), 16);
        assert!(caps.has(KWABI_CAP_ERROR_FIREWALL));
        assert!(
            !caps.has(KWABI_CAP_ATOMIC_BODY),
            "a bit the runtime did not claim must not appear"
        );
    }

    /// `atomic_bodies()` answers the narrow question, not the broad one.
    ///
    /// This is the distinction the bitset exists for: ERROR_FIREWALL says the
    /// error is contained; only ATOMIC_BODY says the writes are undone. An
    /// implementation that made `atomic_bodies()` an alias of the firewall
    /// check would pass a sloppier test and fail this one.
    #[test]
    fn atomic_is_narrower_than_firewall() {
        let firewall_only = Capabilities(KWABI_CAP_CORE | KWABI_CAP_ERROR_FIREWALL);
        assert!(firewall_only.has(KWABI_CAP_ERROR_FIREWALL));
        assert!(
            !firewall_only.atomic_bodies(),
            "containment alone must not imply rollback"
        );

        let both = Capabilities(KWABI_CAP_ERROR_FIREWALL | KWABI_CAP_ATOMIC_BODY);
        assert!(both.atomic_bodies());
    }

    /// `names()` reports exactly the set bits, in order, and `(none)` for zero.
    #[test]
    fn names_reflect_the_bits() {
        let all = Capabilities(
            KWABI_CAP_CORE
                | KWABI_CAP_STRUCTURED_ERRORS
                | KWABI_CAP_ERROR_FIREWALL
                | KWABI_CAP_MEMORY_INTROSPECTION
                | KWABI_CAP_ATOMIC_BODY,
        );
        assert_eq!(
            all.names(),
            "CORE|STRUCTURED_ERRORS|ERROR_FIREWALL|MEMORY_INTROSPECTION|ATOMIC_BODY"
        );
        assert_eq!(Capabilities(0).names(), "(none)");
        assert_eq!(Capabilities(KWABI_CAP_CORE).names(), "CORE");
    }
}
