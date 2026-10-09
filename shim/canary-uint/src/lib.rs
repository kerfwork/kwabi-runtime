//! The uint64 canary's text I/O, as a reloadable type body (kwabi.h "Reloadable type bodies").
//!
//! The parsing and arithmetic are the `uint` core (src/uint.rs). This file is the ABI
//! glue: it fills a KwabiError and a KwabiTypeBodies in the layout the header declares.
//! The output prefix is a compile-time choice, so v1 and v2 are two builds of this source
//! (KWABI_CANARY_PREFIX="" and "u").

#![allow(non_upper_case_globals)]

// The vendored core has more than this body calls; the rest is tested in kwabi-types.
#[allow(dead_code)]
mod uint;

use std::os::raw::{c_char, c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

use uint::{UInt64, UIntError};

const KWABI_ERRMSG_MAX: usize = 512;
const KWABI_ERRDETAIL_MAX: usize = 512;
const KWABI_ERRHINT_MAX: usize = 384;
const KWABI_ERRNAME_MAX: usize = 128;

const KWABI_OK: c_int = 0;
const KWABI_ERR_BODY_RAISED: c_int = 3;
const KWABI_ERR_BAD_ARG: c_int = 4;

const KWABI_TYPE_BODIES_VERSION: u32 = 2;

// Operator codes, mirrored from kwabi.h.
const OP_EQ: u32 = 0;
const OP_NE: u32 = 1;
const OP_LT: u32 = 2;
const OP_LE: u32 = 3;
const OP_GT: u32 = 4;
const OP_GE: u32 = 5;
const OP_ADD: u32 = 6;
const OP_SUB: u32 = 7;
const OP_MUL: u32 = 8;
const OP_DIV: u32 = 9;
const OP_MOD: u32 = 10;
const OP_ORDER: u32 = 11;
const ORDER_LESS: u64 = 0;
const ORDER_EQUAL: u64 = 1;
const ORDER_GREATER: u64 = 2;

// PostgreSQL SQLSTATE encoding (utils/elog.h).
const fn six(ch: u8) -> i32 {
    ((ch - b'0') & 0x3F) as i32
}
const fn sqlstate(c: &[u8; 5]) -> i32 {
    six(c[0]) | (six(c[1]) << 6) | (six(c[2]) << 12) | (six(c[3]) << 18) | (six(c[4]) << 24)
}
const SQLSTATE_INVALID_TEXT: i32 = sqlstate(b"22P02");
const SQLSTATE_OUT_OF_RANGE: i32 = sqlstate(b"22003");
const SQLSTATE_DIVISION_BY_ZERO: i32 = sqlstate(b"22012");

const PREFIX: &str = match option_env!("KWABI_CANARY_PREFIX") {
    Some(p) => p,
    None => "",
};

/// Mirror of `KwabiError` in kwabi.h.
#[repr(C)]
pub struct KwabiError {
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

/// Mirror of `KwabiTypeBodies` in kwabi.h.
#[repr(C)]
pub struct KwabiTypeBodies {
    pub size: u32,
    pub version: u32,
    pub input: unsafe extern "C" fn(*const c_char, *mut u64, *mut KwabiError, *mut c_void) -> c_int,
    pub output:
        unsafe extern "C" fn(u64, *mut c_char, usize, *mut KwabiError, *mut c_void) -> c_int,
    pub binop: unsafe extern "C" fn(u32, u64, u64, *mut u64, *mut KwabiError, *mut c_void) -> c_int,
    pub arg: *mut c_void,
}

// The table holds only function pointers and a null argument, so sharing it is sound.
unsafe impl Sync for KwabiTypeBodies {}

fn fill_error(err: &mut KwabiError, sqlerrcode: i32, status: c_int, message: &str) {
    err.size = std::mem::size_of::<KwabiError>() as u32;
    err.sqlerrcode = sqlerrcode;
    err.status = status;
    err.message = [0; KWABI_ERRMSG_MAX];
    for (dst, src) in err
        .message
        .iter_mut()
        .zip(message.bytes().take(KWABI_ERRMSG_MAX - 1))
    {
        *dst = src as c_char;
    }
}

unsafe fn input_body(text: *const c_char, value: *mut u64, err: *mut KwabiError) -> c_int {
    let text = std::ffi::CStr::from_ptr(text).to_string_lossy();
    match text.parse::<UInt64>() {
        Ok(v) => {
            *value = v.get();
            KWABI_OK
        }
        Err(UIntError::OutOfRange) => {
            fill_error(
                &mut *err,
                SQLSTATE_OUT_OF_RANGE,
                KWABI_ERR_BODY_RAISED,
                "value out of range for type uint64",
            );
            KWABI_ERR_BODY_RAISED
        }
        Err(_) => {
            fill_error(
                &mut *err,
                SQLSTATE_INVALID_TEXT,
                KWABI_ERR_BODY_RAISED,
                "invalid input syntax for type uint64",
            );
            KWABI_ERR_BODY_RAISED
        }
    }
}

unsafe fn output_body(value: u64, buf: *mut c_char, buflen: usize, err: *mut KwabiError) -> c_int {
    let text = format!("{}{}", PREFIX, UInt64::new(value));
    if text.len() + 1 > buflen {
        fill_error(&mut *err, 0, KWABI_ERR_BAD_ARG, "output buffer too small");
        return KWABI_ERR_BAD_ARG;
    }
    std::ptr::copy_nonoverlapping(text.as_ptr() as *const c_char, buf, text.len());
    *buf.add(text.len()) = 0;
    KWABI_OK
}

/// Input. A panic is caught here, because a body must never unwind into the runtime.
unsafe extern "C" fn uint_input(
    text: *const c_char,
    value: *mut u64,
    err: *mut KwabiError,
    _arg: *mut c_void,
) -> c_int {
    catch_unwind(AssertUnwindSafe(|| input_body(text, value, err))).unwrap_or(KWABI_ERR_BODY_RAISED)
}

/// Output. Same containment as input.
unsafe extern "C" fn uint_output(
    value: u64,
    buf: *mut c_char,
    buflen: usize,
    err: *mut KwabiError,
    _arg: *mut c_void,
) -> c_int {
    catch_unwind(AssertUnwindSafe(|| output_body(value, buf, buflen, err)))
        .unwrap_or(KWABI_ERR_BODY_RAISED)
}

/// One operator. Comparisons are 0 or 1; `cmp` is an ordering code; arithmetic is the
/// result. Overflow and division by zero come back as errors, not wrapped values.
fn operator(op: u32, a: u64, b: u64) -> Result<u64, (i32, &'static str)> {
    let (x, y) = (UInt64::new(a), UInt64::new(b));
    let arith = |r: Result<UInt64, UIntError>| -> Result<u64, (i32, &'static str)> {
        r.map(UInt64::get).map_err(|e| match e {
            UIntError::DivideByZero => (SQLSTATE_DIVISION_BY_ZERO, "division by zero"),
            _ => (SQLSTATE_OUT_OF_RANGE, "value out of range for type uint64"),
        })
    };
    Ok(match op {
        OP_EQ => (a == b) as u64,
        OP_NE => (a != b) as u64,
        OP_LT => (a < b) as u64,
        OP_LE => (a <= b) as u64,
        OP_GT => (a > b) as u64,
        OP_GE => (a >= b) as u64,
        OP_ADD => return arith(x.checked_add(y)),
        OP_SUB => return arith(x.checked_sub(y)),
        OP_MUL => return arith(x.checked_mul(y)),
        OP_DIV => return arith(x.checked_div(y)),
        OP_MOD => return arith(x.checked_rem(y)),
        OP_ORDER => match a.cmp(&b) {
            std::cmp::Ordering::Less => ORDER_LESS,
            std::cmp::Ordering::Equal => ORDER_EQUAL,
            std::cmp::Ordering::Greater => ORDER_GREATER,
        },
        _ => return Err((SQLSTATE_INVALID_TEXT, "unknown operator")),
    })
}

unsafe fn binop_body(op: u32, a: u64, b: u64, result: *mut u64, err: *mut KwabiError) -> c_int {
    match operator(op, a, b) {
        Ok(v) => {
            *result = v;
            KWABI_OK
        }
        Err((code, msg)) => {
            fill_error(&mut *err, code, KWABI_ERR_BODY_RAISED, msg);
            KWABI_ERR_BODY_RAISED
        }
    }
}

/// Operators. Same containment as I/O.
unsafe extern "C" fn uint_binop(
    op: u32,
    a: u64,
    b: u64,
    result: *mut u64,
    err: *mut KwabiError,
    _arg: *mut c_void,
) -> c_int {
    catch_unwind(AssertUnwindSafe(|| binop_body(op, a, b, result, err)))
        .unwrap_or(KWABI_ERR_BODY_RAISED)
}

static TYPE_BODIES: KwabiTypeBodies = KwabiTypeBodies {
    size: std::mem::size_of::<KwabiTypeBodies>() as u32,
    version: KWABI_TYPE_BODIES_VERSION,
    input: uint_input,
    output: uint_output,
    binop: uint_binop,
    arg: std::ptr::null_mut(),
};

/// The table the runtime reads at bind time.
#[no_mangle]
pub extern "C" fn kwabi_type_bodies() -> *const KwabiTypeBodies {
    &TYPE_BODIES
}
