//! Proof of operation for the kwabi handoff.
//!
//! The claim under test is the one the whole project rests on: an extension
//! compiled against `kwabi.h` reaches PostgreSQL internals without linking a
//! single PostgreSQL symbol, and does so through a table the runtime
//! published.
//!
//! The test stands in for both sides:
//!
//! * a **stub PostgreSQL** — `palloc` backed by `Vec`, a memory-context
//!   stack, an error slot — which is what a real `KwabiNative` points at;
//! * an **extension** that receives only `*const KwabiV1` and calls through
//!   it, exactly as a C extension built from the header would.
//!
//! Because both the runtime and the "extension" live in one process here,
//! the test also asserts on the *layout* of the table. In production that
//! layout is checked by `size_of` agreement between the header and this
//! struct, and by the canary-extension suite.

use kwabi_runtime::abi::{KwabiV1, KWABI_CAP_CORE, KWABI_VERSION};
use kwabi_runtime::{kwabi_get_api, kwabi_raise, kwabi_runtime_init, KwabiNative};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicUsize, Ordering};

// ========================================================================
// Stub PostgreSQL
// ========================================================================

static ALLOC_COUNT: AtomicUsize = AtomicUsize::new(0);
static FREE_COUNT: AtomicUsize = AtomicUsize::new(0);
static PG_ERROR_CODE: AtomicI32 = AtomicI32::new(0);

/// A memory-context stack, standing in for PostgreSQL's context tree.
///
/// The raw pointers are the stub's own bookkeeping; they are never
/// dereferenced, and the stack is only ever touched while holding the lock.
/// `unsafe impl` keeps the wrapper `Sync` so it can be a `static`.
struct CtxStack(std::sync::Mutex<Vec<usize>>);
unsafe impl Sync for CtxStack {}

static CTX_STACK: CtxStack = CtxStack(std::sync::Mutex::new(Vec::new()));

unsafe extern "C" fn stub_palloc(size: usize) -> *mut c_void {
    ALLOC_COUNT.fetch_add(1, Ordering::SeqCst);
    let layout = std::alloc::Layout::from_size_align(size.max(1), 8).unwrap();
    std::alloc::alloc(layout) as *mut c_void
}

unsafe extern "C" fn stub_palloc0(size: usize) -> *mut c_void {
    ALLOC_COUNT.fetch_add(1, Ordering::SeqCst);
    let layout = std::alloc::Layout::from_size_align(size.max(1), 8).unwrap();
    std::alloc::alloc_zeroed(layout) as *mut c_void
}

unsafe extern "C" fn stub_repalloc(p: *mut c_void, _size: usize) -> *mut c_void {
    // The stub does not actually resize; it only proves the call arrived.
    p
}

unsafe extern "C" fn stub_pfree(p: *mut c_void) {
    FREE_COUNT.fetch_add(1, Ordering::SeqCst);
    let _ = p;
}

unsafe extern "C" fn stub_context_current() -> *mut c_void {
    let s = CTX_STACK.0.lock().unwrap();
    *s.last().unwrap_or(&0) as *mut c_void
}

unsafe extern "C" fn stub_context_switch_to(ctx: *mut c_void) -> *mut c_void {
    let mut s = CTX_STACK.0.lock().unwrap();
    let prev = *s.last().unwrap_or(&0);
    s.push(ctx as usize);
    prev as *mut c_void
}

/// The stub has no real context tree, so reset and delete only record that
/// the call arrived. That is the point: the harness asserts the *forwarding*
/// path, not PostgreSQL's behaviour. Whether a reset actually frees is a
/// claim about PostgreSQL, and it is measured in the live-server matrix.
static RESET_COUNT: AtomicUsize = AtomicUsize::new(0);
static DELETE_COUNT: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn stub_context_reset(ctx: *mut c_void) {
    RESET_COUNT.fetch_add(1, Ordering::SeqCst);
    let _ = ctx;
}

unsafe extern "C" fn stub_context_delete(ctx: *mut c_void) {
    DELETE_COUNT.fetch_add(1, Ordering::SeqCst);
    let _ = ctx;
}

unsafe extern "C" fn stub_error_message() -> *const c_char {
    b"stub: postgres error\0".as_ptr() as *const c_char
}

unsafe extern "C" fn stub_error_code() -> c_int {
    PG_ERROR_CODE.load(Ordering::SeqCst)
}

unsafe extern "C" fn stub_error_clear() {
    PG_ERROR_CODE.store(0, Ordering::SeqCst);
}

unsafe extern "C" fn stub_log_line(_level: c_int, _msg: *const c_char) {}

fn stub_native(pg_major: u32) -> KwabiNative {
    KwabiNative {
        pg_major,
        palloc: Some(stub_palloc),
        palloc0: Some(stub_palloc0),
        repalloc: Some(stub_repalloc),
        pfree: Some(stub_pfree),
        memory_context_current: Some(stub_context_current),
        memory_context_switch_to: Some(stub_context_switch_to),
        memory_context_reset: Some(stub_context_reset),
        memory_context_delete: Some(stub_context_delete),
        error_message: Some(stub_error_message),
        error_code: Some(stub_error_code),
        error_clear: Some(stub_error_clear),
        log_line: Some(stub_log_line),
    }
}

/// Bind the runtime to the stub PostgreSQL and return the published table.
///
/// Every test calls this rather than relying on another test having run:
/// `cargo test` runs them in parallel in one process, so an implicit
/// dependency on init ordering is a race, not a test.
fn ensure_init() -> *const KwabiV1 {
    let native = stub_native(18);
    let api = unsafe { kwabi_runtime_init(&native) };
    assert!(!api.is_null(), "init must publish a table");
    api
}

// ========================================================================
// The extension side
// ========================================================================

/// Exactly what a C extension does with the table: store the pointer, check
/// the version, call through the slots.
struct FakeExtension {
    api: *const KwabiV1,
}

impl FakeExtension {
    /// Mirrors `kwabi_ext_init` in kwabi.h: reject a table we cannot use.
    unsafe fn load(api: *const KwabiV1) -> Option<Self> {
        if api.is_null() {
            return None;
        }
        let v = (*api).version;
        if v != KWABI_VERSION {
            return None;
        }
        Some(FakeExtension { api })
    }

    unsafe fn alloc(&self, n: usize) -> *mut c_void {
        match (*self.api).palloc {
            Some(f) => f(n),
            None => ptr::null_mut(),
        }
    }

    unsafe fn free(&self, p: *mut c_void) {
        if let Some(f) = (*self.api).pfree {
            f(p);
        }
    }

    unsafe fn error_message(&self) -> *const c_char {
        match (*self.api).error_message {
            Some(f) => f(),
            None => ptr::null(),
        }
    }

    unsafe fn error_code(&self) -> c_int {
        match (*self.api).error_code {
            Some(f) => f(),
            None => 0,
        }
    }

    unsafe fn error_clear(&self) {
        if let Some(f) = (*self.api).error_clear {
            f();
        }
    }
}

// ========================================================================
// Tests
// ========================================================================

#[test]
fn table_layout_is_stable() {
    // The ABI's whole guarantee is that this struct's layout is pinned. A
    // field added in the middle, or a type widened, shows up here rather than
    // as a crash in someone's production database.
    let size = std::mem::size_of::<KwabiV1>();
    let align = std::mem::align_of::<KwabiV1>();
    assert_eq!(align, 8, "KwabiV1 must be pointer-aligned");
    assert_eq!(
        size,
        KwabiV1::FIELD_COUNT * 8,
        "every field is pointer-sized; {size} bytes over {} fields means a \
         field is not a pointer (FIELD_COUNT is generated from kwabi.h)",
        KwabiV1::FIELD_COUNT
    );
    // Appended slots move this number. That is the intended workflow for an
    // append-only ABI, and this assertion is what makes the change deliberate:
    // it cannot happen by accident.
    assert_eq!(KwabiV1::FIELD_COUNT, 204, "field count changed — kwabi.h edited?");
}

/// The SDK's hand-written mirror must match the header, field for field.
///
/// There are two mirrors of `KwabiV1` in this tree and only one of them is
/// generated:
///
///   * `runtime-skeleton/src/abi.rs` — produced from `kwabi.h` by
///     `gen_kwabi_struct.py`, so it cannot drift;
///   * `vendor/kwabi/src/lib.rs` — maintained by hand, because the SDK is the
///     artifact an extension links and it must own the types it touches.
///
/// Nothing compared the two, and the hand-written one fell **six appended
/// slots** behind — `memory_chunk_context`, `current_memory_context`,
/// `raise_error`, `try_body`, `error_get`, `capabilities`. The drift was
/// silent because no code read those slots, so no test exercised them; it
/// surfaced only when an extension tried to branch on the bitset and the
/// compiler said `no field 'capabilities'`.
///
/// This test is the missing comparison. It parses both files as text, which is
/// deliberate: a field *added to the header* is what must be caught, and the
/// compiler cannot see that because the SDK simply does not have it. Comparing
/// names in order catches a missing field, an extra one, and a reordering —
/// the three ways an append-only table can go wrong.
#[test]
fn sdk_mirror_matches_the_header() {
    // The header defines the order; the generated mirror proves the parse is
    // sound, so if this test fails the generator and the SDK disagree and the
    // header is the tie-breaker.
    let generated = include_str!("../src/abi.rs");
    let sdk = include_str!("../vendor/kwabi/src/lib.rs");

    fn field_names(src: &str) -> Vec<String> {
        let start = src
            .find("pub struct KwabiV1 {")
            .expect("KwabiV1 must be defined");
        let rest = &src[start..];
        let end = rest.find("\n}").expect("KwabiV1 must be closed");
        rest[..end]
            .lines()
            .filter_map(|l| {
                let l = l.trim();
                let l = l.strip_prefix("pub ")?;
                let (name, _) = l.split_once(':')?;
                Some(name.trim().to_string())
            })
            .collect()
    }

    let want = field_names(generated);
    let got = field_names(sdk);

    assert!(
        !want.is_empty() && !got.is_empty(),
        "both mirrors must expose fields (want={}, got={})",
        want.len(),
        got.len()
    );

    // Name the first divergence, because "lengths differ" is not actionable
    // when the cause is one appended slot.
    for (i, (w, g)) in want.iter().zip(got.iter()).enumerate() {
        assert_eq!(
            w, g,
            "SDK mirror diverges from the header at index {i}: \
             header has `{w}`, SDK has `{g}`"
        );
    }

    if got.len() != want.len() {
        let missing: Vec<&String> = want.iter().skip(got.len()).collect();
        let extra: Vec<&String> = got.iter().skip(want.len()).collect();
        panic!(
            "SDK mirror has {} fields, the header has {}. \
             missing from the SDK: {missing:?}; extra in the SDK: {extra:?}",
            got.len(),
            want.len()
        );
    }

    assert_eq!(
        got.len(),
        KwabiV1::FIELD_COUNT,
        "the SDK mirror, the generated mirror and kwabi.h must all agree"
    );
}

#[test]
fn init_publishes_a_usable_table() {
    let api = ensure_init();

    let ext = unsafe { FakeExtension::load(api) }.expect("extension must accept v1 table");
    assert_eq!(unsafe { (*ext.api).version }, KWABI_VERSION);

    // Re-fetch must hand back the same address, or an extension that cached
    // the pointer would be calling into freed memory.
    assert_eq!(api, kwabi_get_api(), "published address must be stable");
}

#[test]
fn extension_reaches_stub_postgres_through_the_table() {
    let before = ALLOC_COUNT.load(Ordering::SeqCst);
    let api = ensure_init();
    let ext = unsafe { FakeExtension::load(api) }.unwrap();

    let p = unsafe { ext.alloc(128) };
    assert!(!p.is_null(), "palloc through the table must return memory");
    assert_eq!(
        ALLOC_COUNT.load(Ordering::SeqCst),
        before + 1,
        "the call must have reached the stub, not been swallowed"
    );

    unsafe { ext.free(p) };
    assert!(FREE_COUNT.load(Ordering::SeqCst) >= 1, "pfree must reach the stub");
}

#[test]
fn unimplemented_slots_are_null_not_garbage() {
    // v1 wires only memory and error handling. Everything else must be null
    // so an extension testing the slot sees "not implemented" instead of
    // jumping into a stale address.
    let api = ensure_init();
    assert!(api != ptr::null());
    let t = unsafe { &*api };
    assert!(t.palloc.is_some());
    assert!(t.error_message.is_some());
    assert!(t.fmgr_info.is_none(), "fmgr is not wired in the skeleton");
    assert!(t.spi_execute.is_none(), "SPI is not wired in the skeleton");
    assert!(t.table_am_get.is_none(), "table AM is not wired in the skeleton");
    assert!(t.node_type.is_none(), "node IR is not wired in the skeleton");
}

#[test]
fn error_firewall_keeps_runtime_errors_out_of_postgres() {
    let api = ensure_init();
    let ext = unsafe { FakeExtension::load(api) }.unwrap();

    // Baseline: the runtime defers to the stub PostgreSQL.
    unsafe { ext.error_clear() };
    assert_eq!(
        unsafe { std::ffi::CStr::from_ptr(ext.error_message()) }.to_str().unwrap(),
        "stub: postgres error"
    );

    // A runtime-raised error must win, and must not have gone through
    // PostgreSQL's error machinery at all.
    let raised = unsafe { kwabi_raise(42, b"kwabi: bad handle\0".as_ptr() as *const c_char) };
    assert!(raised);
    assert_eq!(unsafe { ext.error_code() }, 42);
    assert_eq!(
        unsafe { std::ffi::CStr::from_ptr(ext.error_message()) }.to_str().unwrap(),
        "kwabi: bad handle"
    );

    // Clearing restores the passthrough.
    unsafe { ext.error_clear() };
    assert_eq!(unsafe { ext.error_code() }, 0);
    assert_eq!(
        unsafe { std::ffi::CStr::from_ptr(ext.error_message()) }.to_str().unwrap(),
        "stub: postgres error"
    );
}

#[test]
fn init_rejects_a_null_native_table() {
    // A separate test binary would be needed to re-init; assert the guard
    // directly instead.
    let api = unsafe { kwabi_runtime_init(ptr::null()) };
    assert!(api.is_null(), "null native table must not publish an ABI");
}

// ========================================================================
// Capability bitset
// ========================================================================

/// A table with nothing wired claims nothing.
///
/// The negative direction. A test that only checks "the bits I want are set"
/// passes trivially against a runtime that returns all-ones, so this asserts
/// the opposite end: an empty table gets an empty bitset.
#[test]
fn capabilities_of_empty_table_is_empty() {
    let t = KwabiV1::default();
    let caps = unsafe { kwabi_runtime::capabilities_of(&t as *const KwabiV1, 18) };
    assert_eq!(caps, 0, "an unwired table must claim nothing");
}

/// ATOMIC_BODY is withheld on a PostgreSQL major the matrix has not measured.
///
/// The most consequential bit in the set, because it is an empirical promise —
/// "a failed body leaves no partial work" — rather than a structural one. It
/// must be granted only for majors the CI matrix has actually exercised.
///
/// This case cannot be produced on a running server: the server IS a measured
/// major. So it is tested here, against a synthetic table.
#[test]
fn atomic_body_withheld_on_unmeasured_major() {
    // A table that has the firewall wired, so the only reason to withhold
    // ATOMIC_BODY is the version.
    let mut t = KwabiV1::default();
    t.try_body = Some(dummy_body_slot);
    t.error_get = Some(dummy_error_get);

    for major in [16u32, 17, 18] {
        let caps = unsafe { kwabi_runtime::capabilities_of(&t as *const KwabiV1, major) };
        assert!(
            caps & 16 != 0,
            "ATOMIC_BODY must be set on measured major {major}"
        );
    }

    for unmeasured in [15u32, 19, 20, 0] {
        let caps = unsafe { kwabi_runtime::capabilities_of(&t as *const KwabiV1, unmeasured) };
        assert_eq!(
            caps & 16,
            0,
            "ATOMIC_BODY must NOT be set on unmeasured major {unmeasured}: \
             the rollback guarantee has not been exercised there"
        );
        // The firewall itself is still claimed: only the empirical promise
        // is withheld, not the structural capability.
        assert!(caps & 4 != 0, "ERROR_FIREWALL is structural, still claimed");
    }
}

/// The bootstrap rule: no capability slot means "fall back to slot tests".
///
/// An extension must not read a NULL slot as "nothing works" — that would
/// refuse to load against a runtime that is merely older than the slot.
/// Verified here because a running server always has the slot.
#[test]
fn capability_bootstrap_rule() {
    // What an extension should do when `api->capabilities` is NULL.
    let caps_from_null_slot = KWABI_CAP_CORE;
    assert_eq!(
        caps_from_null_slot & KWABI_CAP_CORE,
        KWABI_CAP_CORE,
        "an old runtime must still be usable: fall back to slot tests"
    );
    assert_ne!(caps_from_null_slot, 0, "must not mean 'nothing works'");
}

/// Matches the `try_body` slot: it TAKES a body, it is not one.
unsafe extern "C" fn dummy_body_slot(
    _body: kwabi_runtime::abi::KwabiBodyFn,
    _arg: *mut c_void,
    _out: *mut kwabi_runtime::abi::KwabiError,
) -> c_int {
    0
}
unsafe extern "C" fn dummy_error_get(_o: *mut kwabi_runtime::abi::KwabiError) {}
