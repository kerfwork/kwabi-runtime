//! Error firewall: the catching direction.
//!
//! PostgreSQL reports errors by `longjmp`. Rust reports failures by unwinding.
//! Neither survives the other's mechanism, so the firewall has one rule:
//!
//! > **No Rust frame may sit between a `PG_TRY` and a call that can raise.**
//!
//! That single rule forces the whole shape of this module. `PG_TRY` lives in
//! the C shim (`shim/kwabi_runtime_shim.c`), inside each function that can
//! raise. This module's job is the other half: making sure nothing that *can*
//! unwind escapes across the boundary in the other direction.
//!
//! See `notes/error-firewall-design.md` for the measurements behind this.

use crate::abi::{
    KwabiBodyFn, KwabiError, KWABI_ERRDETAIL_MAX, KWABI_ERRHINT_MAX, KWABI_ERRMSG_MAX,
    KWABI_ERRNAME_MAX, KWABI_ERR_BAD_ARG, KWABI_ERR_PANICKED, KWABI_OK,
};
use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

/// The most recent error captured by a guarded body on this backend.
///
/// PostgreSQL is one process per backend and does not run extension entry
/// points concurrently within a backend, so a plain `static mut` is correct
/// here. The accessors below are the only way in or out.
static mut LAST_ERROR: KwabiError = KwabiError {
    size: std::mem::size_of::<KwabiError>() as u32,
    sqlerrcode: 0,
    status: 0,
    message: [0; KWABI_ERRMSG_MAX],
    detail: [0; KWABI_ERRDETAIL_MAX],
    hint: [0; KWABI_ERRHINT_MAX],
    schema_name: [0; KWABI_ERRNAME_MAX],
    table_name: [0; KWABI_ERRNAME_MAX],
    column_name: [0; KWABI_ERRNAME_MAX],
    datatype_name: [0; KWABI_ERRNAME_MAX],
    constraint_name: [0; KWABI_ERRNAME_MAX],
};

/// Copy `s` into a fixed-size buffer, truncating rather than overrunning.
///
/// Truncation is deliberate: an error message is diagnostic text, and a
/// truncated message is more useful than a failed error path. The last byte is
/// always NUL so the result is a valid C string.
fn store_str<const N: usize>(dst: &mut [c_char; N], s: &str) {
    let bytes = s.as_bytes();
    let n = bytes.len().min(N - 1);
    for (i, b) in bytes.iter().take(n).enumerate() {
        dst[i] = *b as c_char;
    }
    dst[n] = 0;
}

/// Read a fixed-size buffer back as a `String`, stopping at the NUL.
pub fn read_str(buf: &[c_char]) -> String {
    let bytes: Vec<u8> = buf
        .iter()
        .take_while(|c| **c != 0)
        .map(|c| *c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

/// Record an error in the runtime's own buffer.
///
/// # Safety
///
/// Single-threaded per backend, as PostgreSQL guarantees for extension code.
pub unsafe fn set_last_error(status: i32, sqlerrcode: i32, msg: &str) {
    set_last_error_full(status, sqlerrcode, msg, "", "");
}

/// Record an error with the optional structured fields.
///
/// # Safety
///
/// Single-threaded per backend.
pub unsafe fn set_last_error_full(
    status: i32,
    sqlerrcode: i32,
    msg: &str,
    detail: &str,
    hint: &str,
) {
    let e = &mut *std::ptr::addr_of_mut!(LAST_ERROR);
    e.status = status;
    e.sqlerrcode = sqlerrcode;
    store_str(&mut e.message, msg);
    store_str(&mut e.detail, detail);
    store_str(&mut e.hint, hint);
}

/// Read the runtime's last error.
///
/// # Safety
///
/// Single-threaded per backend.
pub unsafe fn last_error() -> KwabiError {
    let e = &*std::ptr::addr_of!(LAST_ERROR);
    KwabiError {
        size: e.size,
        sqlerrcode: e.sqlerrcode,
        status: e.status,
        message: e.message,
        detail: e.detail,
        hint: e.hint,
        schema_name: e.schema_name,
        table_name: e.table_name,
        column_name: e.column_name,
        datatype_name: e.datatype_name,
        constraint_name: e.constraint_name,
    }
}

/// Write the runtime's error into a caller-provided struct, respecting its size.
///
/// # The size guard, and why it is not optional
///
/// The caller's struct may be **smaller** than this runtime's. An extension
/// built against an older header passes its own `KwabiError`, and at the ABI
/// level both are just `KwabiError *` — the compiler cannot tell them apart.
/// Writing the full struct would overflow the caller's buffer.
///
/// So every write is guarded by the caller's declared `size`, and fields that
/// do not fit are skipped. The caller gets the core fields and misses the
/// optional ones, which is the correct degradation: better than an overflow,
/// and better than refusing to report an error at all.
///
/// Measured: without these guards a v1 caller (524 bytes) suffers a 57-byte
/// overrun from a v2 writer (2060 bytes). With them, zero. See
/// `runtime-skeleton/errsize/`.
///
/// # Safety
///
/// `out` must be null, or point to a writable `KwabiError` (possibly a smaller
/// versioned one) whose `size` field accurately describes the allocation.
pub unsafe fn write_error_to(out: *mut KwabiError, err: &KwabiError) {
    if out.is_null() {
        return;
    }
    let caller_size = (*out).size as usize;

    // The core fields sit at identical offsets in every version of the struct,
    // so they can be written whenever the caller's struct could hold them.
    let core_end = std::mem::offset_of!(KwabiError, message) + 32;
    if caller_size < core_end {
        return;
    }
    (*out).sqlerrcode = err.sqlerrcode;
    (*out).status = err.status;
    (*out).message = err.message;

    // Optional fields: each written only if the caller's struct extends to it.
    macro_rules! guarded {
        ($field:ident) => {
            if caller_size
                >= std::mem::offset_of!(KwabiError, $field)
                    + std::mem::size_of_val(&err.$field)
            {
                (*out).$field = err.$field;
            }
        };
    }
    guarded!(detail);
    guarded!(hint);
    guarded!(schema_name);
    guarded!(table_name);
    guarded!(column_name);
    guarded!(datatype_name);
    guarded!(constraint_name);
}

/// Overwrite the runtime's error buffer from a fully-populated `KwabiError`.
///
/// A plain byte copy: both sides are the same `#[repr(C)]` type. Used when the
/// error is captured in C — the shim's fmgr slots do this — rather than by a
/// guarded body. The shim can build a `KwabiError` with the header's own
/// helpers but cannot reach the runtime's private buffer; this is the bridge.
///
/// # Safety
///
/// Single-threaded per backend, as PostgreSQL guarantees for extension code.
pub unsafe fn set_last_error_from(e: &KwabiError) {
    let dst = std::ptr::addr_of_mut!(LAST_ERROR);
    std::ptr::copy_nonoverlapping(e as *const KwabiError, dst, 1);
}

/// Run `body`, containing any panic it raises.
///
/// This is the *inner* half of the firewall. It does not touch PostgreSQL
/// errors at all — those are the shim's job. It exists purely so that a panic
/// cannot reach an `extern "C"` boundary.
///
/// # Why the panic must be caught here and not outside
///
/// Since Rust 1.81 a panic escaping an `extern "C"` function aborts the
/// process. Wrapping `catch_unwind` *around* a call to an `extern "C"`
/// function therefore catches nothing: the panic dies at that function's own
/// boundary before the outer guard ever sees it. Verified on 1.99 — see
/// `notes/error-firewall-design.md` §4.
///
/// Note the honest scope: this contains panics from the *runtime's own* code.
/// A panic inside an extension is a foreign exception to this std and cannot be
/// caught here — which is why `#[guarded_body]` exists, catching it in the
/// extension's own crate. See `notes/panic-abort-dx.md`.
///
/// # Safety
///
/// `body` must be a valid function pointer and `arg` must be valid for it.
pub unsafe fn guard_body(body: KwabiBodyFn, arg: *mut c_void, out: *mut KwabiError) -> i32 {
    // `AssertUnwindSafe` is correct rather than a shrug: the body is an
    // `extern "C"` function taking a raw pointer, so it shares no Rust state
    // with this frame. There is nothing for an unwind to leave half-updated.
    let result = catch_unwind(AssertUnwindSafe(|| body(arg, out)));

    match result {
        Ok(status) => status,
        Err(payload) => {
            let msg = if let Some(s) = payload.downcast_ref::<&str>() {
                (*s).to_string()
            } else if let Some(s) = payload.downcast_ref::<String>() {
                s.clone()
            } else {
                "panic with a non-string payload".to_string()
            };

            set_last_error(
                KWABI_ERR_PANICKED,
                0,
                &format!("kwabi: runtime panic: {}", msg),
            );
            KWABI_ERR_PANICKED
        }
    }
}

/// Validate the arguments a caller passed to `try_body`.
///
/// # Safety
///
/// `out` may be null.
pub unsafe fn check_try_args(out: *mut KwabiError) -> i32 {
    if out.is_null() {
        set_last_error(KWABI_ERR_BAD_ARG, 0, "kwabi: try_body out-param is null");
        return KWABI_ERR_BAD_ARG;
    }
    KWABI_OK
}
