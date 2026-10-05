//! A guarded body that panics.
//!
//! This exists to demonstrate what the contract forbids and why. A body that
//! panics aborts the backend: the panic dies at this function's own `extern "C"`
//! boundary, before any guard can see it. That is the body's bug, and it is
//! contained to one backend rather than corrupting shared state.
//!
//! It is kept in the tree because the *reason* it cannot be contained is
//! subtle -- a panic from a separately built extension is a foreign exception
//! to the runtime's Rust std, and `catch_unwind` refuses it. See
//! notes/error-firewall-design.md section 4.
//!
//! Built into libcanary as `canary_panicking_body`. The body is `extern "C"`
//! per the kwabi.h contract, and it panics — which the contract does not
//! forbid. Panics are contained by the runtime's catch_unwind; *raises* are
//! what the contract forbids.

use std::os::raw::c_void;

#[no_mangle]
pub extern "C" fn canary_panicking_body(_arg: *mut c_void) -> i32 {
    panic!("canary: deliberate panic from a guarded body");
}
