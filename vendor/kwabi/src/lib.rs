//! Safe Rust bindings for the kwabi stable ABI for PostgreSQL.
//!
//! This crate provides safe, idiomatic Rust wrappers over the kwabi C ABI.
//! Extensions use this crate to interact with PostgreSQL without unsafe code.
//!
//! # Example
//!
//! An extension's SQL-callable body. The `#[guarded_body]` macro generates the
//! `extern "C"` trampoline that the runtime's `try_body` calls, and wraps the
//! body so a panic is caught inside the extension's own Rust std rather than
//! crossing the ABI.
//!
//! ```rust
//! use kwabi::guarded::KwabiReport;
//!
//! // The body's argument type is the extension's own: it is whatever the
//! // caller passes through `arg`. A common shape is a struct of parameters
//! // that the trampoline hands over by pointer.
//! #[derive(Default)]
//! pub struct MyArgs {
//!     pub should_fail: i32,
//! }
//!
//! fn my_body(args: &mut MyArgs) -> Result<(), KwabiReport> {
//!     if args.should_fail != 0 {
//!         return Err(KwabiReport::new("22023", "value out of range")
//!             .detail("Allowed range is 1 to 100.")
//!             .hint("Check the extension's configuration.")
//!             .column("amount"));
//!     }
//!     Ok(())
//! }
//!
//! # let mut a = MyArgs::default();
//! # assert!(my_body(&mut a).is_ok());
//! # a.should_fail = 1;
//! # let e = my_body(&mut a).unwrap_err();
//! # assert_eq!(e.sqlstate, "22023");
//! # assert_eq!(e.column_name, "amount");
//! ```

pub mod guarded;

/// Re-export the `#[guarded_body]` attribute macro.
pub use kwabi_macros::{guarded_body, require_unwind};

use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

const KWABI_VERSION_1: u32 = 1;

// ========================================================================
// Raw FFI types (matching kwabi.h)
// ========================================================================

#[repr(C)]
pub struct KwabiV1 {
    pub version: u32,
    // Function calls
    //
    // `Option<unsafe extern "C" fn>`, not a bare pointer: the header's rule is
    // that every slot starts null and an extension MUST test a slot before
    // calling it, and a bare function-pointer field cannot express absence.
    // These five are wired by the per-version shim (they can raise, and the
    // error firewall forbids a Rust frame between a PG_TRY and a raising call).
    //
    // The signatures carry `*mut bool isnull` and `*mut u64 result` rather than
    // returning a bare `Datum`: `Datum` has no null channel, and PostgreSQL
    // signals a NULL result out-of-band through `fcinfo->isnull`. A bare return
    // would make NULL indistinguishable from 0.
    pub fmgr_info: Option<unsafe extern "C" fn(u32) -> *mut KwabiFmgrInfo>,
    pub call_function: Option<
        unsafe extern "C" fn(
            *mut KwabiFmgrInfo,
            i32,
            *mut u64,
            *const bool,
            *mut bool,
            *mut u64,
        ) -> i32,
    >,
    pub call_function1:
        Option<unsafe extern "C" fn(*mut KwabiFmgrInfo, u64, *mut bool, *mut u64) -> i32>,
    pub call_function2:
        Option<unsafe extern "C" fn(*mut KwabiFmgrInfo, u64, u64, *mut bool, *mut u64) -> i32>,
    pub call_function3:
        Option<unsafe extern "C" fn(*mut KwabiFmgrInfo, u64, u64, u64, *mut bool, *mut u64) -> i32>,
    // SPI
    //
    // `Option<unsafe extern "C" fn>`, not a bare pointer: the header's rule is
    // that every slot starts null and an extension MUST test a slot before
    // calling it, and a bare function-pointer field cannot express absence.
    // These five are wired by the per-version shim (they can raise, and the
    // error firewall forbids a Rust frame between a PG_TRY and a raising call).
    pub spi_execute: Option<unsafe extern "C" fn(*const c_char, bool, i32) -> *mut KwabiSPIResult>,
    pub spi_execute_plan: Option<
        unsafe extern "C" fn(
            *mut KwabiSPIPlan,
            *mut u64,
            *const c_char,
            bool,
            i32,
        ) -> *mut KwabiSPIResult,
    >,
    pub spi_free_result: Option<unsafe extern "C" fn(*mut KwabiSPIResult)>,
    pub spi_result_ntuples: Option<unsafe extern "C" fn(*mut KwabiSPIResult) -> i32>,
    pub spi_result_get_value: Option<unsafe extern "C" fn(*mut KwabiSPIResult, i32, i32) -> u64>,
    // Type system
    pub type_input: extern "C" fn(u32, *const c_char, i32) -> u64,
    pub type_output: extern "C" fn(u32, u64) -> *mut c_char,
    pub type_recv: extern "C" fn(u32, *mut std::ffi::c_void) -> u64,
    pub type_send: extern "C" fn(u32, u64, *mut std::ffi::c_void),
    pub type_element_type: extern "C" fn(u32) -> u32,
    pub type_length: extern "C" fn(u32) -> i16,
    pub type_is_array: extern "C" fn(u32) -> bool,
    pub type_is_composite: extern "C" fn(u32) -> bool,
    pub type_base_type: extern "C" fn(u32) -> u32,
    // Parser
    pub parse_expr: extern "C" fn(*const c_char, *mut u32, i32) -> *mut KwabiNode,
    pub parse_stmt: extern "C" fn(*const c_char) -> *mut KwabiNode,
    pub parse_type: extern "C" fn(*const c_char) -> *mut KwabiNode,
    pub free_node: extern "C" fn(*mut KwabiNode),
    pub oper_left_type: extern "C" fn(u32) -> u32,
    pub oper_right_type: extern "C" fn(u32) -> u32,
    pub oper_result_type: extern "C" fn(u32) -> u32,
    pub oper_is_commutative: extern "C" fn(u32) -> bool,
    // Commands
    pub extension_oid: extern "C" fn(*const c_char) -> u32,
    pub extension_installed: extern "C" fn(*const c_char) -> bool,
    pub extension_version: extern "C" fn(*const c_char) -> *const c_char,
    pub sequence_nextval: extern "C" fn(u32) -> i64,
    pub sequence_currval: extern "C" fn(u32) -> i64,
    pub sequence_setval: extern "C" fn(u32, i64) -> i64,
    // Replication
    pub logical_decoding_begin: extern "C" fn(u32, i64) -> *mut KwabiLogicalDecodingCtx,
    pub logical_decoding_end: extern "C" fn(*mut KwabiLogicalDecodingCtx),
    pub logical_decoding_read:
        extern "C" fn(*mut KwabiLogicalDecodingCtx, *mut i64, *mut *mut std::ffi::c_void) -> bool,
    pub output_plugin_startup: extern "C" fn(*mut std::ffi::c_void),
    pub output_plugin_shutdown: extern "C" fn(*mut std::ffi::c_void),
    // Background workers
    pub bgworker_register: extern "C" fn(
        *const c_char,
        extern "C" fn(*mut std::ffi::c_void),
        *mut std::ffi::c_void,
    ) -> u32,
    pub bgworker_terminate: extern "C" fn(u32),
    pub bgworker_is_running: extern "C" fn(u32) -> bool,
    // Storage primitives
    pub block_get_number: extern "C" fn(*mut std::ffi::c_void) -> u32,
    pub block_get_offset: extern "C" fn(*mut std::ffi::c_void) -> u16,
    pub block_is_valid: extern "C" fn(*mut std::ffi::c_void) -> bool,
    pub slru_create: extern "C" fn(*const c_char, i32, i32),
    pub slru_read: extern "C" fn(*const c_char, i64, *mut std::ffi::c_void),
    pub slru_write: extern "C" fn(*const c_char, i64, *const std::ffi::c_void),
    // Value nodes
    pub value_is_null: extern "C" fn(*mut KwabiValue) -> bool,
    pub value_get_datum: extern "C" fn(*mut KwabiValue) -> u64,
    pub value_get_type: extern "C" fn(*mut KwabiValue) -> u32,
    pub value_get_typmod: extern "C" fn(*mut KwabiValue) -> i32,
    // Memory contexts
    pub palloc: extern "C" fn(usize) -> *mut std::ffi::c_void,
    pub palloc0: extern "C" fn(usize) -> *mut std::ffi::c_void,
    pub repalloc: extern "C" fn(*mut std::ffi::c_void, usize) -> *mut std::ffi::c_void,
    pub pfree: extern "C" fn(*mut std::ffi::c_void),
    pub memory_context_current: extern "C" fn() -> *mut KwabiMemoryContext,
    pub memory_context_switch_to: extern "C" fn(*mut KwabiMemoryContext) -> *mut KwabiMemoryContext,
    pub memory_context_reset: Option<unsafe extern "C" fn(*mut KwabiMemoryContext)>,
    pub memory_context_delete: Option<unsafe extern "C" fn(*mut KwabiMemoryContext)>,
    // Error handling
    pub ereport: extern "C" fn(i32, *const c_char, ...),
    pub elog: extern "C" fn(i32, *const c_char, ...),
    pub error_message: extern "C" fn() -> *const c_char,
    pub error_code: extern "C" fn() -> i32,
    pub error_clear: extern "C" fn(),
    // Relation cache
    pub relation_open: extern "C" fn(u32, u32) -> *mut KwabiRelation,
    pub relation_close: extern "C" fn(*mut KwabiRelation, u32),
    pub relation_id: extern "C" fn(*mut KwabiRelation) -> u32,
    pub relation_name: extern "C" fn(*mut KwabiRelation) -> *const c_char,
    pub relation_namespace: extern "C" fn(*mut KwabiRelation) -> u32,
    pub relation_tupledesc: extern "C" fn(*mut KwabiRelation) -> *mut std::ffi::c_void,
    // System cache
    pub syscache_get_oid: extern "C" fn(*const c_char, *const c_char, u64) -> u32,
    pub syscache_get_tuple: extern "C" fn(*const c_char, u64) -> *mut std::ffi::c_void,
    pub syscache_free_tuple: extern "C" fn(*mut std::ffi::c_void),
    // Optimizer
    pub planner_info:
        extern "C" fn(*mut KwabiNode, i32, *mut std::ffi::c_void) -> *mut KwabiPlannerInfo,
    pub free_planner_info: extern "C" fn(*mut KwabiPlannerInfo),
    pub planner_estimate_rows: extern "C" fn(*mut KwabiPlannerInfo, *mut std::ffi::c_void) -> f64,
    pub planner_estimate_cost: extern "C" fn(*mut KwabiPlannerInfo, *mut std::ffi::c_void) -> f64,
    // Transactions
    pub transaction_start: extern "C" fn(),
    pub transaction_commit: extern "C" fn(),
    pub transaction_abort: extern "C" fn(),
    pub transaction_is_active: extern "C" fn() -> bool,
    pub transaction_get_current_xid: extern "C" fn() -> i64,
    // Storage
    pub shmem_alloc: extern "C" fn(usize) -> *mut std::ffi::c_void,
    pub shmem_free: extern "C" fn(*mut std::ffi::c_void),
    pub shmem_get: extern "C" fn(*const c_char, usize) -> *mut std::ffi::c_void,
    pub lock_acquire: extern "C" fn(*mut std::ffi::c_void, u32),
    pub lock_release: extern "C" fn(*mut std::ffi::c_void),
    pub lock_held_by_me: extern "C" fn(*mut std::ffi::c_void) -> bool,
    pub spin_acquire: extern "C" fn(*mut std::ffi::c_void),
    pub spin_release: extern "C" fn(*mut std::ffi::c_void),
    // Postmaster
    pub autovacuum_is_running: extern "C" fn() -> bool,
    pub autovacuum_naptime: extern "C" fn() -> i32,
    pub syslogger_log: extern "C" fn(*const c_char),
    // WAL replication
    pub walsender_send: extern "C" fn(*const c_char, i32),
    pub walsender_receive: extern "C" fn(*mut c_char, i32) -> i32,
    pub walsender_is_connected: extern "C" fn() -> bool,
    // Commands (defrem)
    //
    // `Option<unsafe extern "C" fn>`, not a bare pointer: the header's rule is
    // that every slot starts null and an extension MUST test a slot before
    // calling it, and a bare function-pointer field cannot express absence.
    // These three are wired by the per-version shim (they can raise, and the
    // error firewall forbids a Rust frame between a PG_TRY and a raising call).
    pub defrem_create: Option<unsafe extern "C" fn(*const c_char, *const c_char, *const c_char)>,
    pub defrem_alter: Option<unsafe extern "C" fn(*const c_char, *const c_char)>,
    pub defrem_drop: Option<unsafe extern "C" fn(*const c_char)>,
    // Node trees
    pub node_type: extern "C" fn(*mut KwabiNode) -> u32,
    pub node_type_name: extern "C" fn(*mut KwabiNode) -> *const c_char,
    pub node_get_list: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub node_list_length: extern "C" fn(*mut KwabiNode) -> i32,
    pub node_list_get: extern "C" fn(*mut KwabiNode, i32) -> *mut KwabiNode,
    pub query_command_type: extern "C" fn(*mut KwabiNode) -> u32,
    pub query_rtable: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_target_list: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_returning_list: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_jointree: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_group_clause: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_sort_clause: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_limit_offset: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_limit_count: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub query_has_for_update: extern "C" fn(*mut KwabiNode) -> bool,
    pub query_has_row_security: extern "C" fn(*mut KwabiNode) -> bool,
    pub planned_stmt_plan_tree: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub planned_stmt_rtable: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub planned_stmt_result_relations: extern "C" fn(*mut KwabiNode) -> *mut std::ffi::c_void,
    pub planned_stmt_has_returning: extern "C" fn(*mut KwabiNode) -> bool,
    pub planned_stmt_has_modifying_cte: extern "C" fn(*mut KwabiNode) -> bool,
    pub planned_stmt_is_utility: extern "C" fn(*mut KwabiNode) -> bool,
    // Tuples
    pub tuple_natts: extern "C" fn(*mut std::ffi::c_void) -> i32,
    pub tuple_typeid: extern "C" fn(*mut std::ffi::c_void, i32) -> u32,
    pub tuple_typmod: extern "C" fn(*mut std::ffi::c_void, i32) -> i32,
    pub tuple_attname: extern "C" fn(*mut std::ffi::c_void, i32) -> *const c_char,
    pub tuple_attisdropped: extern "C" fn(*mut std::ffi::c_void, i32) -> bool,
    pub tuple_attnum: extern "C" fn(*mut std::ffi::c_void, *const c_char) -> i32,
    pub heap_tuple_getattr:
        extern "C" fn(*mut std::ffi::c_void, i32, *mut std::ffi::c_void, *mut bool) -> u64,
    pub heap_tuple_setattr: extern "C" fn(
        *mut std::ffi::c_void,
        i32,
        u64,
        *mut std::ffi::c_void,
    ) -> *mut std::ffi::c_void,
    pub heap_tuple_tableoid: extern "C" fn(*mut std::ffi::c_void) -> u32,
    pub heap_tuple_tid: extern "C" fn(*mut std::ffi::c_void) -> *mut std::ffi::c_void,
    pub slot_isnull: extern "C" fn(*mut std::ffi::c_void, i32) -> bool,
    pub slot_getattr: extern "C" fn(*mut std::ffi::c_void, i32, *mut bool) -> u64,
    pub slot_tupledesc: extern "C" fn(*mut std::ffi::c_void) -> *mut std::ffi::c_void,
    // Table AM
    pub table_am_get: extern "C" fn(u32) -> *mut KwabiTableAm,
    pub table_am_beginscan: extern "C" fn(
        *mut KwabiTableAm,
        *mut std::ffi::c_void,
        i32,
        *mut std::ffi::c_void,
    ) -> *mut std::ffi::c_void,
    pub table_am_endscan: extern "C" fn(*mut std::ffi::c_void),
    pub table_am_getnext: extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void) -> bool,
    pub table_am_insert:
        extern "C" fn(*mut KwabiTableAm, *mut std::ffi::c_void, i32, *mut std::ffi::c_void),
    pub table_am_update: extern "C" fn(*mut KwabiTableAm, *mut std::ffi::c_void, i32),
    pub table_am_delete: extern "C" fn(*mut KwabiTableAm, *mut std::ffi::c_void, i32),
    // Executor
    pub executor_start: extern "C" fn(*mut std::ffi::c_void, i32) -> *mut KwabiEState,
    pub executor_run: extern "C" fn(*mut KwabiEState, i32, i64, bool),
    pub executor_finish: extern "C" fn(*mut KwabiEState),
    pub executor_end: extern "C" fn(*mut KwabiEState),
    pub executor_getnext: extern "C" fn(*mut KwabiEState) -> *mut std::ffi::c_void,
    // Buffer manager
    pub buffer_get: extern "C" fn(*mut std::ffi::c_void, u32) -> *mut std::ffi::c_void,
    pub buffer_release: extern "C" fn(*mut std::ffi::c_void),
    pub buffer_get_page: extern "C" fn(*mut std::ffi::c_void) -> *mut std::ffi::c_void,
    pub buffer_mark_dirty: extern "C" fn(*mut std::ffi::c_void),
    // Locks
    pub lwlock_acquire: extern "C" fn(*mut std::ffi::c_void, u32),
    pub lwlock_release: extern "C" fn(*mut std::ffi::c_void),
    pub lwlock_held_by_me: extern "C" fn(*mut std::ffi::c_void) -> bool,
    pub lwlock_cond_acquire: extern "C" fn(*mut std::ffi::c_void, u32) -> bool,
    pub spinlock_acquire: extern "C" fn(*mut std::ffi::c_void),
    pub spinlock_release: extern "C" fn(*mut std::ffi::c_void),
    pub spinlock_held_by_me: extern "C" fn(*mut std::ffi::c_void) -> bool,
    // GUC
    //
    // `Option<unsafe extern "C" fn>`, not a bare pointer: the header's rule is
    // that every slot starts null and an extension MUST test a slot before
    // calling it, and a bare function-pointer field cannot express absence.
    // These eight are wired by the per-version shim (they can raise, and the
    // error firewall forbids a Rust frame between a PG_TRY and a raising call).
    pub guc_get_int: Option<unsafe extern "C" fn(*const c_char) -> i32>,
    pub guc_get_string: Option<unsafe extern "C" fn(*const c_char) -> *const c_char>,
    pub guc_get_bool: Option<unsafe extern "C" fn(*const c_char) -> bool>,
    pub guc_get_float: Option<unsafe extern "C" fn(*const c_char) -> f64>,
    pub guc_set_int: Option<unsafe extern "C" fn(*const c_char, i32)>,
    pub guc_set_string: Option<unsafe extern "C" fn(*const c_char, *const c_char)>,
    pub guc_set_bool: Option<unsafe extern "C" fn(*const c_char, bool)>,
    pub guc_set_float: Option<unsafe extern "C" fn(*const c_char, f64)>,
    // Explain
    pub explain_query: extern "C" fn(
        *mut std::ffi::c_void,
        *mut std::ffi::c_void,
        *mut std::ffi::c_void,
        *const c_char,
        *mut std::ffi::c_void,
        *mut std::ffi::c_void,
    ),
    pub explain_get_index_name: extern "C" fn(u32) -> *const c_char,
    // Vacuum
    pub vacuum_rel:
        extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void, *mut std::ffi::c_void),
    pub vacuum_analyze_rel:
        extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void, *mut std::ffi::c_void),
    // Triggers
    pub trigger_desc: extern "C" fn(*mut std::ffi::c_void) -> *mut std::ffi::c_void,
    pub trigger_count: extern "C" fn(*mut std::ffi::c_void) -> i32,
    pub trigger_get: extern "C" fn(*mut std::ffi::c_void, i32) -> *mut std::ffi::c_void,
    // Replication internals
    pub reorderbuffer_get_lsn: extern "C" fn(*mut KwabiReorderBuffer) -> i64,
    pub reorderbuffer_get_xid: extern "C" fn(*mut KwabiReorderBuffer, u32) -> i64,
    pub reorderbuffer_get_changes: extern "C" fn(*mut KwabiReorderBuffer, u32) -> i32,
    pub slot_get_lsn: extern "C" fn(u32) -> i64,
    pub slot_get_catalog_xmin: extern "C" fn(u32) -> i64,
    pub slot_is_active: extern "C" fn(u32) -> bool,
    // Postmaster
    pub postmaster_is_alive: extern "C" fn() -> bool,
    pub postmaster_get_child_pid: extern "C" fn(u32) -> i32,
    // Item pointers
    pub itempointer_get_block_number: extern "C" fn(*mut std::ffi::c_void) -> u32,
    pub itempointer_get_offset_number: extern "C" fn(*mut std::ffi::c_void) -> u16,
    pub itempointer_is_valid: extern "C" fn(*mut std::ffi::c_void) -> bool,
    // Relations
    pub rel_id: extern "C" fn(*mut KwabiRelation) -> u32,
    pub rel_name: extern "C" fn(*mut KwabiRelation) -> *const c_char,
    pub rel_namespace: extern "C" fn(*mut KwabiRelation) -> u32,
    pub rel_relkind: extern "C" fn(*mut KwabiRelation) -> i8,
    pub rel_relam: extern "C" fn(*mut KwabiRelation) -> u32,
    pub rel_tupledesc: extern "C" fn(*mut KwabiRelation) -> *mut std::ffi::c_void,
    pub rel_index_list: extern "C" fn(*mut KwabiRelation) -> *mut std::ffi::c_void,
    // String info
    pub stringinfo_init: extern "C" fn(*mut std::ffi::c_void),
    pub stringinfo_reset: extern "C" fn(*mut std::ffi::c_void),
    pub stringinfo_append: extern "C" fn(*mut std::ffi::c_void, *const c_char),
    pub stringinfo_append_char: extern "C" fn(*mut std::ffi::c_void, i8),
    pub stringinfo_append_int: extern "C" fn(*mut std::ffi::c_void, i64),
    pub stringinfo_data: extern "C" fn(*mut std::ffi::c_void) -> *const c_char,
    pub stringinfo_len: extern "C" fn(*mut std::ffi::c_void) -> i32,

    // ---- appended after stringinfo (mirrors kwabi.h) ---------------------
    //
    // These six are `Option` where every field above is a bare function
    // pointer, and the difference is load-bearing rather than stylistic.
    //
    // A bare function-pointer field cannot express "this slot is absent":
    // calling a null one jumps to address 0. But the header's rule is that
    // every slot starts null and an extension MUST test a slot before calling
    // it — so a field that cannot be tested cannot honour the rule. That was
    // survivable while every slot was one the extension chose to call; it is
    // not survivable for `capabilities`, whose whole contract is "a NULL slot
    // means no capability query, fall back to slot tests". The bootstrap rule
    // is unrepresentable in a bare pointer.
    //
    // `Option<fn>` is the same size and layout as the pointer it wraps (null
    // == None), so this costs nothing at the ABI level and is checked against
    // the header's field count by the harness like every other field.
    pub memory_chunk_context:
        Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> *mut std::ffi::c_void>,
    pub current_memory_context: Option<unsafe extern "C" fn() -> *mut std::ffi::c_void>,
    pub raise_error: Option<unsafe extern "C" fn(i32, *const c_char)>,
    pub try_body: Option<
        unsafe extern "C" fn(
            guarded::KwabiBodyFn,
            *mut std::ffi::c_void,
            *mut guarded::KwabiErrorAbi,
        ) -> i32,
    >,
    pub error_get: Option<unsafe extern "C" fn(*mut guarded::KwabiErrorAbi)>,
    pub capabilities: Option<unsafe extern "C" fn() -> u64>,
    pub memory_context_create:
        Option<unsafe extern "C" fn(*const c_char) -> *mut KwabiMemoryContext>,
}

// Opaque handle types
#[repr(C)]
pub struct KwabiRelation {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiNode {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiValue {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiTableAm {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiEState {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiMemoryContext {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiPlannerInfo {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiLogicalDecodingCtx {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiReorderBuffer {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiFmgrInfo {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiSPIResult {
    _private: [u8; 0],
}
#[repr(C)]
pub struct KwabiSPIPlan {
    _private: [u8; 0],
}

// ========================================================================
// Safe wrapper types
// ========================================================================

/// The main kwabi handle. All PostgreSQL access goes through this.
pub struct Kwabi {
    api: &'static KwabiV1,
}

/// A relation (table) handle.
pub struct Relation<'a> {
    handle: *mut KwabiRelation,
    kwabi: &'a Kwabi,
}

/// A node (query/plan) handle.
pub struct Node<'a> {
    handle: *mut KwabiNode,
    kwabi: &'a Kwabi,
}

/// A SPI result handle.
pub struct SPIResult<'a> {
    handle: *mut KwabiSPIResult,
    kwabi: &'a Kwabi,
}

/// A memory context handle.
pub struct MemoryContext<'a> {
    handle: *mut KwabiMemoryContext,
    kwabi: &'a Kwabi,
}

/// A looked-up function handle (`FmgrInfo`).
///
/// Owned by the memory context it was allocated in, so it has no `Drop`: the
/// context frees it. Borrow it for the duration of a call, not beyond the
/// context's life.
pub struct FmgrInfo {
    handle: *mut KwabiFmgrInfo,
}

impl FmgrInfo {
    /// The raw handle, for the `call_function*` slots.
    pub fn handle(&self) -> *mut KwabiFmgrInfo {
        self.handle
    }
}

/// A logical decoding context handle.
pub struct LogicalDecodingCtx<'a> {
    handle: *mut KwabiLogicalDecodingCtx,
    kwabi: &'a Kwabi,
}

// ========================================================================
// Error handling
// ========================================================================

/// A kwabi error.
#[derive(Debug)]
pub struct KwabiError {
    pub code: i32,
    pub message: String,
}

impl std::fmt::Display for KwabiError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "kwabi error {}: {}", self.code, self.message)
    }
}

impl std::error::Error for KwabiError {}

// ========================================================================
// Kwabi implementation
// ========================================================================

impl Kwabi {
    /// Create a new Kwabi instance from the function table.
    ///
    /// This is called by the runtime at load time. Extensions should not
    /// call this directly.
    pub fn from_api(api: &'static KwabiV1) -> Self {
        Kwabi { api }
    }

    /// Get the ABI version.
    pub fn version(&self) -> u32 {
        self.api.version
    }

    // ---- Capabilities ----

    /// What this runtime GUARANTEES, as distinct from which slots exist.
    ///
    /// This is the accessor a body calls before relying on a promise. The
    /// question it exists to answer: a non-NULL `try_body` says the runtime
    /// can *contain* an error; only `ATOMIC_BODY` says a failed body's partial
    /// writes are *rolled back*. An extension that writes needs the second
    /// answer and cannot get it by testing the slot.
    ///
    /// Applies the bootstrap rule rather than exposing the raw slot, so a
    /// caller cannot get it wrong: a NULL slot yields `CORE`-only (fall back
    /// to testing slots directly), NOT zero. Reading it as zero would make an
    /// extension refuse to load against a runtime that is merely older than
    /// the slot — the opposite of what this ABI is for.
    ///
    /// `pg_major` is unused when the slot is present, which is the normal
    /// case; it is carried so the runtime's own answer and the SDK's
    /// fallback have the same shape.
    pub fn capabilities(&self, pg_major: u32) -> guarded::Capabilities {
        guarded::capabilities_from_slot(self.api.capabilities, pg_major)
    }

    // ---- Function calls (fmgr) ----

    /// Look up a function by OID.
    ///
    /// `None` if the runtime does not provide `fmgr_info`, or the lookup
    /// failed — the latter is a real PostgreSQL error and is readable through
    /// `Kwabi::error()`. The `FmgrInfo` is allocated in the current memory
    /// context, so it lives as long as that context does.
    pub fn fmgr_info(&self, fn_oid: u32) -> Option<FmgrInfo> {
        let f = self.api.fmgr_info?;
        let handle = unsafe { f(fn_oid) };
        if handle.is_null() {
            return None;
        }
        Some(FmgrInfo { handle })
    }

    /// Call a 0-argument function.
    ///
    /// `Ok(None)` means the function returned SQL NULL — a normal outcome, not
    /// an error. `Err(())` means the call did not complete; read the error with
    /// `Kwabi::error()`. (The error type is `()` rather than `KwabiError`
    /// because the message lives in the runtime's buffer, not here; call
    /// `error()` to fetch it.)
    #[allow(clippy::result_unit_err)]
    pub fn call_function0(&self, info: &FmgrInfo) -> Result<Option<u64>, ()> {
        self.call_function(info, &[], &[])
    }

    /// Call a 1-argument function.
    #[allow(clippy::result_unit_err)]
    pub fn call_function1(&self, info: &FmgrInfo, arg1: u64) -> Result<Option<u64>, ()> {
        self.call_function(info, &[arg1], &[false])
    }

    /// Call a 2-argument function.
    #[allow(clippy::result_unit_err)]
    pub fn call_function2(&self, info: &FmgrInfo, arg1: u64, arg2: u64) -> Result<Option<u64>, ()> {
        self.call_function(info, &[arg1, arg2], &[false, false])
    }

    /// Call a 3-argument function.
    #[allow(clippy::result_unit_err)]
    pub fn call_function3(
        &self,
        info: &FmgrInfo,
        arg1: u64,
        arg2: u64,
        arg3: u64,
    ) -> Result<Option<u64>, ()> {
        self.call_function(info, &[arg1, arg2, arg3], &[false, false, false])
    }

    /// Call a function with N arguments, honouring per-argument NULLs.
    ///
    /// `argnulls` must be the same length as `args`. A NULL argument is passed
    /// through as NULL; note that `FunctionCallInvoke` does not short-circuit a
    /// `strict` function, so the function itself sees the NULL.
    ///
    /// This goes through the variadic `call_function` slot, which the shim
    /// implements by building the `FunctionCallInfo` itself — PostgreSQL has no
    /// N-ary call helper.
    #[allow(clippy::result_unit_err)]
    pub fn call_function(
        &self,
        info: &FmgrInfo,
        args: &[u64],
        argnulls: &[bool],
    ) -> Result<Option<u64>, ()> {
        debug_assert_eq!(args.len(), argnulls.len());
        let f = match self.api.call_function {
            Some(f) => f,
            None => return Err(()),
        };
        let mut isnull = false;
        let mut result: u64 = 0;
        let n = args.len().min(i32::MAX as usize) as i32;
        let status = unsafe {
            f(
                info.handle,
                n,
                args.as_ptr() as *mut u64,
                argnulls.as_ptr(),
                &mut isnull,
                &mut result,
            )
        };
        if status != 0 {
            return Err(());
        }
        if isnull {
            Ok(None)
        } else {
            Ok(Some(result))
        }
    }

    /// The runtime's last error, as a message string.
    pub fn error(&self) -> String {
        let ptr = (self.api.error_message)();
        if ptr.is_null() {
            return String::new();
        }
        unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() }
    }

    // ---- Memory ----

    /// Allocate memory in the current memory context.
    pub fn palloc(&self, size: usize) -> *mut u8 {
        (self.api.palloc)(size) as *mut u8
    }

    /// Allocate zeroed memory in the current memory context.
    pub fn palloc0(&self, size: usize) -> *mut u8 {
        (self.api.palloc0)(size) as *mut u8
    }

    /// Reallocate memory.
    pub fn repalloc(&self, ptr: *mut u8, size: usize) -> *mut u8 {
        (self.api.repalloc)(ptr as *mut _, size) as *mut u8
    }

    /// Free memory.
    pub fn pfree(&self, ptr: *mut u8) {
        (self.api.pfree)(ptr as *mut _);
    }

    // ---- SPI ----

    /// Execute a SQL query.
    ///
    /// Returns `Err` if the runtime does not provide `spi_execute`, or the
    /// query failed. The SPI slots are wired by the per-version shim.
    pub fn spi_query(&self, sql: &str) -> Result<SPIResult<'_>, KwabiError> {
        let c_sql = CString::new(sql).map_err(|_| KwabiError {
            code: -1,
            message: "invalid SQL string".to_string(),
        })?;

        let f = match self.api.spi_execute {
            Some(f) => f,
            None => {
                return Err(KwabiError {
                    code: -1,
                    message: "spi_execute is not wired".to_string(),
                })
            }
        };

        let result = catch_unwind(AssertUnwindSafe(|| unsafe { f(c_sql.as_ptr(), false, 0) }))
            .map_err(|_| KwabiError {
                code: -1,
                message: "panic in spi_execute".to_string(),
            })?;

        if result.is_null() {
            return Err(KwabiError {
                code: -1,
                message: "spi_execute returned null".to_string(),
            });
        }

        Ok(SPIResult {
            handle: result,
            kwabi: self,
        })
    }

    /// Execute a read-only SQL query.
    pub fn spi_query_read_only(&self, sql: &str) -> Result<SPIResult<'_>, KwabiError> {
        let c_sql = CString::new(sql).map_err(|_| KwabiError {
            code: -1,
            message: "invalid SQL string".to_string(),
        })?;

        let f = match self.api.spi_execute {
            Some(f) => f,
            None => {
                return Err(KwabiError {
                    code: -1,
                    message: "spi_execute is not wired".to_string(),
                })
            }
        };

        let result = catch_unwind(AssertUnwindSafe(|| unsafe { f(c_sql.as_ptr(), true, 0) }))
            .map_err(|_| KwabiError {
                code: -1,
                message: "panic in spi_execute".to_string(),
            })?;

        if result.is_null() {
            return Err(KwabiError {
                code: -1,
                message: "spi_execute returned null".to_string(),
            });
        }

        Ok(SPIResult {
            handle: result,
            kwabi: self,
        })
    }

    // ---- Relations ----

    /// Open a relation (table).
    pub fn relation_open(&self, relid: u32) -> Result<Relation<'_>, KwabiError> {
        let handle = catch_unwind(AssertUnwindSafe(|| {
            (self.api.relation_open)(relid, 1) // KWABI_LOCKMODE_SHARE
        }))
        .map_err(|_| KwabiError {
            code: -1,
            message: "panic in relation_open".to_string(),
        })?;

        if handle.is_null() {
            return Err(KwabiError {
                code: -1,
                message: "relation_open returned null".to_string(),
            });
        }

        Ok(Relation {
            handle,
            kwabi: self,
        })
    }

    // ---- Transactions ----

    /// Start a transaction.
    pub fn transaction_start(&self) {
        (self.api.transaction_start)();
    }

    /// Commit the current transaction.
    pub fn transaction_commit(&self) {
        (self.api.transaction_commit)();
    }

    /// Abort the current transaction.
    pub fn transaction_abort(&self) {
        (self.api.transaction_abort)();
    }

    /// Check if a transaction is active.
    pub fn transaction_is_active(&self) -> bool {
        (self.api.transaction_is_active)()
    }

    // ---- GUC ----

    /// Get an integer GUC value.
    ///
    /// Returns `Err` if the runtime does not provide `guc_get_int`.
    pub fn guc_int(&self, name: &str) -> Result<i32, KwabiError> {
        let c_name = CString::new(name).map_err(|_| KwabiError {
            code: -1,
            message: "invalid GUC name".to_string(),
        })?;
        let f = match self.api.guc_get_int {
            Some(f) => f,
            None => {
                return Err(KwabiError {
                    code: -1,
                    message: "guc_get_int is not wired".to_string(),
                })
            }
        };
        Ok(unsafe { f(c_name.as_ptr()) })
    }

    /// Get a string GUC value.
    ///
    /// Returns `Err` if the runtime does not provide `guc_get_string`.
    pub fn guc_string(&self, name: &str) -> Result<String, KwabiError> {
        let c_name = CString::new(name).map_err(|_| KwabiError {
            code: -1,
            message: "invalid GUC name".to_string(),
        })?;
        let f = match self.api.guc_get_string {
            Some(f) => f,
            None => {
                return Err(KwabiError {
                    code: -1,
                    message: "guc_get_string is not wired".to_string(),
                })
            }
        };
        let ptr = unsafe { f(c_name.as_ptr()) };
        if ptr.is_null() {
            return Err(KwabiError {
                code: -1,
                message: "guc_get_string returned null".to_string(),
            });
        }
        unsafe {
            let c_str = CStr::from_ptr(ptr);
            Ok(c_str.to_string_lossy().into_owned())
        }
    }

    /// Get a boolean GUC value.
    ///
    /// Returns `Err` if the runtime does not provide `guc_get_bool`.
    pub fn guc_bool(&self, name: &str) -> Result<bool, KwabiError> {
        let c_name = CString::new(name).map_err(|_| KwabiError {
            code: -1,
            message: "invalid GUC name".to_string(),
        })?;
        let f = match self.api.guc_get_bool {
            Some(f) => f,
            None => {
                return Err(KwabiError {
                    code: -1,
                    message: "guc_get_bool is not wired".to_string(),
                })
            }
        };
        Ok(unsafe { f(c_name.as_ptr()) })
    }

    // ---- Extensions ----

    /// Check if an extension is installed.
    pub fn extension_installed(&self, extname: &str) -> Result<bool, KwabiError> {
        let c_name = CString::new(extname).map_err(|_| KwabiError {
            code: -1,
            message: "invalid extension name".to_string(),
        })?;
        Ok((self.api.extension_installed)(c_name.as_ptr()))
    }

    /// Get an extension's OID.
    pub fn extension_oid(&self, extname: &str) -> Result<u32, KwabiError> {
        let c_name = CString::new(extname).map_err(|_| KwabiError {
            code: -1,
            message: "invalid extension name".to_string(),
        })?;
        Ok((self.api.extension_oid)(c_name.as_ptr()))
    }

    // ---- Sequences ----

    /// Get the next value from a sequence.
    pub fn sequence_nextval(&self, seq_oid: u32) -> Result<i64, KwabiError> {
        Ok((self.api.sequence_nextval)(seq_oid))
    }

    /// Get the current value of a sequence.
    pub fn sequence_currval(&self, seq_oid: u32) -> Result<i64, KwabiError> {
        Ok((self.api.sequence_currval)(seq_oid))
    }

    // ---- Background workers ----

    /// Register a background worker.
    pub fn bgworker_register(
        &self,
        name: &str,
        main: extern "C" fn(*mut std::ffi::c_void),
        arg: *mut std::ffi::c_void,
    ) -> Result<u32, KwabiError> {
        let c_name = CString::new(name).map_err(|_| KwabiError {
            code: -1,
            message: "invalid worker name".to_string(),
        })?;
        Ok((self.api.bgworker_register)(c_name.as_ptr(), main, arg))
    }

    // ---- Locks ----

    /// Acquire a lightweight lock.
    pub fn lwlock_acquire(&self, lock: *mut std::ffi::c_void, mode: u32) {
        (self.api.lock_acquire)(lock as *mut _, mode);
    }

    /// Release a lightweight lock.
    pub fn lwlock_release(&self, lock: *mut std::ffi::c_void) {
        (self.api.lock_release)(lock as *mut _);
    }

    // ---- Shared memory ----

    /// Allocate shared memory.
    pub fn shmem_alloc(&self, size: usize) -> *mut u8 {
        (self.api.shmem_alloc)(size) as *mut u8
    }

    /// Free shared memory.
    pub fn shmem_free(&self, ptr: *mut u8) {
        (self.api.shmem_free)(ptr as *mut _);
    }

    // ---- StringInfo ----

    /// Initialize a StringInfo.
    pub fn stringinfo_init(&self, str: *mut std::ffi::c_void) {
        (self.api.stringinfo_init)(str as *mut _);
    }

    /// Reset a StringInfo.
    pub fn stringinfo_reset(&self, str: *mut std::ffi::c_void) {
        (self.api.stringinfo_reset)(str as *mut _);
    }

    /// Append to a StringInfo.
    pub fn stringinfo_append(&self, str: *mut std::ffi::c_void, data: &str) {
        if let Ok(c_data) = CString::new(data) {
            (self.api.stringinfo_append)(str as *mut _, c_data.as_ptr());
        }
    }

    // ---- Type system ----

    /// Get the element type of an array type.
    pub fn type_element_type(&self, type_oid: u32) -> Result<u32, KwabiError> {
        Ok((self.api.type_element_type)(type_oid))
    }

    /// Get the length of a type.
    pub fn type_length(&self, type_oid: u32) -> Result<i16, KwabiError> {
        Ok((self.api.type_length)(type_oid))
    }

    /// Check if a type is an array.
    pub fn type_is_array(&self, type_oid: u32) -> Result<bool, KwabiError> {
        Ok((self.api.type_is_array)(type_oid))
    }

    // ---- Parser ----

    /// Parse a SQL expression.
    pub fn parse_expr(&self, sql: &str) -> Result<Node<'_>, KwabiError> {
        let c_sql = CString::new(sql).map_err(|_| KwabiError {
            code: -1,
            message: "invalid SQL string".to_string(),
        })?;

        let handle = catch_unwind(AssertUnwindSafe(|| {
            (self.api.parse_expr)(c_sql.as_ptr(), ptr::null_mut(), 0)
        }))
        .map_err(|_| KwabiError {
            code: -1,
            message: "panic in parse_expr".to_string(),
        })?;

        if handle.is_null() {
            return Err(KwabiError {
                code: -1,
                message: "parse_expr returned null".to_string(),
            });
        }

        Ok(Node {
            handle,
            kwabi: self,
        })
    }
}

// ========================================================================
// Relation implementation
// ========================================================================

impl<'a> Relation<'a> {
    /// Get the relation's OID.
    pub fn oid(&self) -> u32 {
        (self.kwabi.api.relation_id)(self.handle)
    }

    /// Get the relation's name.
    pub fn name(&self) -> String {
        unsafe {
            let ptr = (self.kwabi.api.relation_name)(self.handle);
            if ptr.is_null() {
                return String::new();
            }
            CStr::from_ptr(ptr).to_string_lossy().into_owned()
        }
    }

    /// Get the relation's namespace OID.
    pub fn namespace(&self) -> u32 {
        (self.kwabi.api.relation_namespace)(self.handle)
    }

    /// Get the relation's tuple descriptor.
    pub fn tupdesc(&self) -> *mut std::ffi::c_void {
        (self.kwabi.api.relation_tupledesc)(self.handle) as *mut _
    }

    /// Get the number of attributes.
    pub fn natts(&self) -> i32 {
        let tupdesc = self.tupdesc();
        if tupdesc.is_null() {
            return 0;
        }
        (self.kwabi.api.tuple_natts)(tupdesc)
    }
}

impl<'a> Drop for Relation<'a> {
    fn drop(&mut self) {
        (self.kwabi.api.relation_close)(self.handle, 1); // KWABI_LOCKMODE_SHARE
    }
}

// ========================================================================
// Node implementation
// ========================================================================

impl<'a> Node<'a> {
    /// Get the node type.
    pub fn node_type(&self) -> u32 {
        (self.kwabi.api.node_type)(self.handle)
    }

    /// Get the node type name.
    pub fn type_name(&self) -> String {
        unsafe {
            let ptr = (self.kwabi.api.node_type_name)(self.handle);
            if ptr.is_null() {
                return String::new();
            }
            CStr::from_ptr(ptr).to_string_lossy().into_owned()
        }
    }

    /// Get the number of list elements.
    pub fn list_length(&self) -> i32 {
        (self.kwabi.api.node_list_length)(self.handle)
    }

    /// Get a list element.
    pub fn list_get(&self, index: i32) -> Option<Node<'a>> {
        let handle = (self.kwabi.api.node_list_get)(self.handle, index);
        if handle.is_null() {
            return None;
        }
        Some(Node {
            handle,
            kwabi: self.kwabi,
        })
    }
}

impl<'a> Drop for Node<'a> {
    fn drop(&mut self) {
        (self.kwabi.api.free_node)(self.handle);
    }
}

// ========================================================================
// SPIResult implementation
// ========================================================================

impl<'a> SPIResult<'a> {
    /// Get the number of tuples.
    ///
    /// Returns 0 if the runtime does not provide `spi_result_ntuples`.
    pub fn ntuples(&self) -> i32 {
        match self.kwabi.api.spi_result_ntuples {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// Get a value from the result.
    ///
    /// Returns 0 if the runtime does not provide `spi_result_get_value`.
    pub fn get_value(&self, tupno: i32, attno: i32) -> u64 {
        match self.kwabi.api.spi_result_get_value {
            Some(f) => unsafe { f(self.handle, tupno, attno) },
            None => 0,
        }
    }
}

impl<'a> Drop for SPIResult<'a> {
    fn drop(&mut self) {
        if let Some(f) = self.kwabi.api.spi_free_result {
            unsafe { f(self.handle) };
        }
    }
}

// ========================================================================
// MemoryContext implementation
// ========================================================================

impl<'a> MemoryContext<'a> {
    /// Create a new memory context under the current one.
    ///
    /// Returns `None` if the runtime does not provide `memory_context_create`.
    /// The name is required — anonymous contexts are a debugging dead end.
    pub fn new(kwabi: &'a Kwabi, name: &str) -> Option<Self> {
        let name_c = std::ffi::CString::new(name).ok()?;
        let f = kwabi.api.memory_context_create?;
        let handle = unsafe { f(name_c.as_ptr()) };
        if handle.is_null() {
            return None;
        }
        Some(Self { handle, kwabi })
    }

    /// Reset the memory context.
    pub fn reset(&self) {
        if let Some(f) = self.kwabi.api.memory_context_reset {
            unsafe { f(self.handle) };
        }
    }
}

impl<'a> Drop for MemoryContext<'a> {
    fn drop(&mut self) {
        if let Some(f) = self.kwabi.api.memory_context_delete {
            unsafe { f(self.handle) };
        }
    }
}

// ========================================================================
// LogicalDecodingCtx implementation
// ========================================================================

impl<'a> LogicalDecodingCtx<'a> {
    /// Read the next logical decoding change.
    pub fn read(&self) -> Option<(i64, *mut std::ffi::c_void)> {
        let mut lsn: i64 = 0;
        let mut data: *mut std::ffi::c_void = ptr::null_mut();
        let success = (self.kwabi.api.logical_decoding_read)(self.handle, &mut lsn, &mut data);
        if success {
            Some((lsn, data))
        } else {
            None
        }
    }
}

impl<'a> Drop for LogicalDecodingCtx<'a> {
    fn drop(&mut self) {
        (self.kwabi.api.logical_decoding_end)(self.handle);
    }
}

// ========================================================================
// Extension entry point
// ========================================================================

/// The extension entry point. The runtime calls this at load time.
///
/// # Safety
///
/// This function is called by the runtime and should not be called directly.
#[no_mangle]
pub unsafe extern "C" fn kwabi_ext_init(api: *const KwabiV1) -> bool {
    if api.is_null() {
        return false;
    }

    let api_ref = &*api;
    if api_ref.version < KWABI_VERSION_1 {
        return false;
    }

    // Store the function table in a global
    KWABI_API = Some(api_ref);

    true
}

static mut KWABI_API: Option<&'static KwabiV1> = None;

/// Get the global Kwabi instance.
///
/// This is called by the runtime after `kwabi_ext_init`.
pub fn instance() -> Option<Kwabi> {
    unsafe { KWABI_API.map(Kwabi::from_api) }
}

// ========================================================================
// Nightly features
// ========================================================================

#[cfg(feature = "nightly")]
pub mod nightly {
    //! Nightly-only features that require unstable Rust features.
    //!
    //! These features are only available when the `nightly` feature is enabled
    //! and the crate is built with a nightly Rust compiler.

    /// A wrapper around `std::panic::catch_unwind` that provides better
    /// error messages on nightly.
    pub fn catch_unwind<F, R>(f: F) -> std::thread::Result<R>
    where
        F: FnOnce() -> R,
    {
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(f))
    }
}

// ========================================================================
// Tests
// ========================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version() {
        // This test requires a runtime to be loaded
        // In practice, the runtime calls kwabi_ext_init and then instance()
    }

    #[test]
    fn test_error_display() {
        let err = KwabiError {
            code: 42,
            message: "test error".to_string(),
        };
        assert_eq!(format!("{}", err), "kwabi error 42: test error");
    }
}
