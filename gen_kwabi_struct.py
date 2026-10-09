#!/usr/bin/env python3
"""
gen_kwabi_struct.py — generate the Rust mirror of KwabiV1 from kwabi.h.

The header is the ABI contract. Hand-copying ~130 function-pointer fields into
Rust guarantees drift, so we parse the header and emit the struct. Field order
is preserved exactly, and the harness asserts on size_of::<KwabiV1>() so any
layout drift fails a test instead of corrupting memory in production.

Usage: python3 gen_kwabi_struct.py kwabi.h > src/abi.rs
"""

import re
import sys

# Scalar C types -> Rust.
SCALARS = {
    "uint32_t": "u32",
    "uint64_t": "u64",
    "int32_t": "i32",
    "int64_t": "i64",
    "int16_t": "i16",
    "uint16_t": "u16",
    "uint8_t": "u8",
    "int8_t": "i8",
    "size_t": "usize",
    "int": "c_int",
    "long": "c_long",
    "bool": "bool",
    "float": "f32",
    "double": "f64",
    "char": "c_char",
    "void": "c_void",
    # kwabi scalar typedefs
    "Oid": "u32",
    "TransactionId": "u32",
    "BlockNumber": "u32",
    "OffsetNumber": "u16",
    "Datum": "usize",
    "LOCKMODE": "u32",
    "LWLockMode": "u32",
    "CmdType": "u32",
    "KwabiNodeType": "u32",
    "KwabiCmdType": "u32",
    "KwabiLockMode": "u32",
    "KwabiLWLockMode": "u32",
    "int32": "i32",
    "int64": "i64",
    "int16": "i16",
    "uint32": "u32",
    "uint64": "u64",
    "uint8": "u8",
    "int8": "i8",
    "float4": "f32",
    "float8": "f64",
    # Error-firewall enums: plain ints at the ABI. An enum in a C function
    # pointer signature has implementation-defined width, so the ABI treats it
    # as int and the generated struct says so.
    "KwabiStatus": "c_int",
}

# Types the generated file defines itself. These are emitted by name so the
# Rust struct is typed rather than flattened to void *, which is the difference
# between a compiler-checked ABI and a convention.
VERBATIM = {
    "KwabiError": "KwabiError",
    "KwabiBodyFn": "KwabiBodyFn",
    "KwabiExecutorStartBody": "KwabiExecutorStartBody",
    "KwabiExecutorRunBody": "KwabiExecutorRunBody",
    "KwabiExecutorFinishBody": "KwabiExecutorFinishBody",
    "KwabiExecutorEndBody": "KwabiExecutorEndBody",
}

# Opaque handle typedefs -> the Rust pointee they actually are.
# `typedef void *KwabiRelation;` means KwabiRelation is already one pointer.
HANDLES = {
    "KwabiRelation": "c_void",
    "KwabiNode": "c_void",
    "KwabiValue": "c_void",
    "KwabiTableAm": "c_void",
    "KwabiEState": "c_void",
    "KwabiMemoryContext": "c_void",
    "KwabiPlannerInfo": "c_void",
    "KwabiLogicalDecodingCtx": "c_void",
    "KwabiReorderBuffer": "c_void",
    "KwabiFmgrInfo": "c_void",
    "KwabiSPIResult": "c_void",
    "KwabiSPIPlan": "c_void",
    "KwabiOutputPluginCallbacks": "c_void",
    "KwabiList": "c_void",
    "KwabiPlan": "c_void",
    "KwabiSlot": "c_void",
    "HeapTuple": "c_void",
    "TupleDesc": "c_void",
    "TupleTableSlot": "c_void",
    "TableScanDesc": "c_void",
    "Buffer": "c_void",
    "Page": "c_void",
    "ItemPointer": "c_void",
    "TriggerDesc": "c_void",
    "Trigger": "c_void",
    "BulkInsertState": "c_void",
    "StringInfo": "c_void",
    "List": "c_void",
    "Node": "c_void",
    "Plan": "c_void",
    "KwabiQueryDesc": "c_void",
    "QueryDesc": "c_void",
    "ParamListInfo": "c_void",
    "Snapshot": "c_void",
    "IntoClause": "c_void",
    "ExplainState": "c_void",
    "QueryEnvironment": "c_void",
    "KwabiHookNext": "c_void",
    "KwabiIntoClause": "c_void",
    "KwabiExplainState": "c_void",
    "KwabiParamListInfo": "c_void",
    "KwabiQueryEnvironment": "c_void",
    "KwabiSnapshot": "c_void",
    "ScanKey": "c_void",
    "VacuumParams": "c_void",
    "BufferAccessStrategy": "c_void",
    "Relation": "c_void",
    "LWLock": "c_void",
    "slock_t": "c_void",
    "BackendId": "c_void",
    "MemoryContext": "c_void",
    "bgworker_main_type": "c_void",
    # KwabiError is a real struct the extension owns; pass it by pointer.
    "KwabiError": "c_void",
    # KwabiBodyFn is a function pointer, already one level of indirection.
    "KwabiBodyFn": "c_void",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def split_args(args_c):
    depth = 0
    cur = []
    for ch in args_c:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            yield "".join(cur)
            cur = []
        else:
            cur.append(ch)
    yield "".join(cur)


def resolve(c_type):
    """C type expression -> Rust type, handling const and pointer levels."""
    t = " ".join(c_type.split())
    is_const = "const" in t.split() or t.startswith("const ")
    t = t.replace("const", "").strip()

    # Count trailing pointer stars.
    stars = 0
    while t.endswith("*"):
        stars += 1
        t = t[:-1].strip()

    base = t.strip()
    if base == "" or base == "void":
        # bare void* / void
        if stars == 0:
            return "()" if not is_const else "()"
        return ("*const " if is_const else "*mut ") + "c_void" if stars == 1 else "*mut c_void"

    if base in VERBATIM:
        # A named type defined in the generated file: keep it, add the stars.
        out = VERBATIM[base]
        for i in range(stars):
            c = is_const and i == 0
            out = ("*const " if c else "*mut ") + out
        return out

    if base in HANDLES:
        # Handle is already one pointer to c_void; add the extra stars.
        total = stars + 1
        out = "c_void"
        for _ in range(total):
            out = ("*const " if is_const else "*mut ") + out
            is_const = False
        return out

    if base in SCALARS:
        rust = SCALARS[base]
        if stars == 0:
            return rust
        out = rust
        for i in range(stars):
            c = is_const and i == 0
            out = ("*const " if c else "*mut ") + out
        return out

    # Unknown base: pass it through as an identifier, pointing at c_void.
    out = base
    for i in range(stars):
        c = is_const and i == 0
        out = ("*const " if c else "*mut ") + out
    return out


def rust_args_for(args_c):
    args_c = args_c.strip()
    if args_c in ("", "void"):
        return ""
    out = []
    for arg in split_args(args_c):
        arg = arg.strip()
        if not arg or arg == "void":
            continue
        if arg == "...":
            # Rust function-pointer fields cannot be variadic. Represent the
            # varargs slot as a single pointer and route real varargs through
            # the runtime wrapper; the doc comment on the field notes it.
            out.append("*const c_char")
            continue
        # Array parameter decays to a pointer.
        arr = re.match(r"^(.*?)\s+(\w+)\s*\[\s*\]$", arg)
        if arr:
            out.append(resolve(arr.group(1).strip() + " *"))
            continue
        # Function-pointer parameter: T (*name)(ARGS)
        fp = re.match(r"^(.*?)\s*\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$", arg, flags=re.S)
        if fp:
            out.append("Option<unsafe extern \"C\" fn()>")
            continue
        # Strip the trailing parameter name.
        m = re.match(r"^(.*?)\s*(\w+)$", arg, flags=re.S)
        base = m.group(1).strip() if m else arg
        out.append(resolve(base))
    return ", ".join(out)


def parse_struct(header_text):
    body = re.search(
        r"typedef\s+struct\s+KwabiV1\s*\{(.*?)\}\s*KwabiV1\s*;",
        header_text,
        flags=re.S,
    )
    if not body:
        sys.exit("error: could not find 'typedef struct KwabiV1 { ... } KwabiV1;'")
    body = body.group(1)

    fields = []
    pending = None
    for raw in body.split(";"):
        decl = raw.strip()
        if not decl:
            continue
        cm = re.search(r"/\*\s*(.*?)\s*\*/", raw, flags=re.S)
        comment = " ".join(cm.group(1).split()) if cm else None
        if cm:
            decl = raw[: cm.start()].strip()
        if not decl:
            if comment:
                pending = comment
            continue

        fn = re.match(r"^(.*?)\s*\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$", decl, flags=re.S)
        if fn:
            ret_c, name, args_c = fn.group(1).strip(), fn.group(2), fn.group(3).strip()
            variadic = "..." in args_c
            rust_ret = resolve(ret_c)
            rust_args = rust_args_for(args_c)
            if variadic and pending:
                pending = pending + " (variadic: passed as a single pointer)"
            fields.append((f"Option<unsafe extern \"C\" fn({rust_args}) -> {rust_ret}>", name, pending))
            pending = None
            continue

        m = re.match(r"^(.*?)\s+(\w+)$", decl, flags=re.S)
        if not m:
            sys.exit(f"error: cannot parse declaration: {decl!r}")
        fields.append((resolve(m.group(1)), m.group(2), pending))
        pending = None

    return fields


def emit(fields):
    L = []
    L.append("// @generated by gen_kwabi_struct.py from kwabi.h — do not edit by hand.")
    L.append("//")
    L.append("// The layout of this struct IS the ABI. Field order must match kwabi.h")
    L.append("// exactly; the harness asserts on size_of::<KwabiV1>() so drift fails a")
    L.append("// test rather than corrupting memory in production.")
    L.append("")
    L.append("use std::os::raw::{c_char, c_int, c_long, c_void};")
    L.append("")
    L.append("// ---- capabilities (mirrors kwabi.h) ----")
    L.append("")
    L.append("/// A capability bit says what a runtime GUARANTEES, not which slots")
    L.append("/// exist. A NULL slot cannot express \"implemented, but with a weaker")
    L.append("/// guarantee than you need\"; these bits can. See kwabi.h.")
    L.append("///")
    L.append("/// Hard rule: a bit MUST NOT be derivable from a slot test. If testing a")
    L.append("/// slot gives the same answer, the bit is noise.")
    L.append("pub const KWABI_CAP_CORE: u64 = 1 << 0;")
    L.append("pub const KWABI_CAP_STRUCTURED_ERRORS: u64 = 1 << 1;")
    L.append("pub const KWABI_CAP_ERROR_FIREWALL: u64 = 1 << 2;")
    L.append("pub const KWABI_CAP_MEMORY_INTROSPECTION: u64 = 1 << 3;")
    L.append("pub const KWABI_CAP_ATOMIC_BODY: u64 = 1 << 4;")
    L.append("pub const KWABI_CAP_SLRU: u64 = 1 << 5;")
    L.append("pub const KWABI_CAP_HOOKS: u64 = 1 << 6;")
    L.append("pub const KWABI_CAP_ALL: u64 = KWABI_CAP_CORE")
    L.append("    | KWABI_CAP_STRUCTURED_ERRORS")
    L.append("    | KWABI_CAP_ERROR_FIREWALL")
    L.append("    | KWABI_CAP_MEMORY_INTROSPECTION")
    L.append("    | KWABI_CAP_ATOMIC_BODY")
    L.append("    | KWABI_CAP_SLRU")
    L.append("    | KWABI_CAP_HOOKS;")
    L.append("")
    L.append("/// Stable ABI version published by this runtime.")
    L.append("pub const KWABI_VERSION: u32 = 1;")
    L.append("")
    L.append("// ---- error firewall types (mirrors kwabi.h) ----")
    L.append("")
    L.append("/// Status codes. `KWABI_OK` is 0 so an integer check reads naturally.")
    L.append("pub const KWABI_OK: c_int = 0;")
    L.append("pub const KWABI_ERR_RAISED: c_int = 1;")
    L.append("pub const KWABI_ERR_PANICKED: c_int = 2;")
    L.append("pub const KWABI_ERR_BODY_RAISED: c_int = 3;")
    L.append("pub const KWABI_ERR_BAD_ARG: c_int = 4;")
    L.append("")
    L.append("/// Field sizes. Must match the KWABI_ERR*_MAX defines in kwabi.h.")
    L.append("pub const KWABI_ERRMSG_MAX: usize = 512;")
    L.append("pub const KWABI_ERRDETAIL_MAX: usize = 512;")
    L.append("pub const KWABI_ERRHINT_MAX: usize = 384;")
    L.append("pub const KWABI_ERRNAME_MAX: usize = 128;")
    L.append("")
    L.append("/// Smallest `KwabiError` that can carry the extended fields.")
    L.append("///")
    L.append("/// The capability bit for structured errors is set only when the struct")
    L.append("/// we compiled against is at least this large, so the bit cannot be")
    L.append("/// claimed by a build whose struct has no detail/hint/object fields.")
    L.append("pub const KWABI_ERR_STRUCTURED_MIN: usize =")
    L.append("    4 + 4 + 4 + KWABI_ERRMSG_MAX + KWABI_ERRDETAIL_MAX + KWABI_ERRHINT_MAX")
    L.append("        + 5 * KWABI_ERRNAME_MAX;")
    L.append("")
    L.append("/// The structured error channel.")
    L.append("///")
    L.append("/// `size` is first and must stay first: it is what lets a writer built")
    L.append("/// against a newer header refuse to write past a caller's older, smaller")
    L.append("/// struct. See the long comment in kwabi.h for why that case is the normal")
    L.append("/// one rather than an edge case.")
    L.append("///")
    L.append("/// All strings are fixed arrays, so the struct has no ownership question:")
    L.append("/// it crosses by value and can be copied or zeroed freely.")
    L.append("#[repr(C)]")
    L.append("pub struct KwabiError {")
    L.append("    pub size: u32,")
    L.append("    pub sqlerrcode: c_int,")
    L.append("    pub status: c_int,")
    L.append("    pub message: [c_char; KWABI_ERRMSG_MAX],")
    L.append("    pub detail: [c_char; KWABI_ERRDETAIL_MAX],")
    L.append("    pub hint: [c_char; KWABI_ERRHINT_MAX],")
    L.append("    pub schema_name: [c_char; KWABI_ERRNAME_MAX],")
    L.append("    pub table_name: [c_char; KWABI_ERRNAME_MAX],")
    L.append("    pub column_name: [c_char; KWABI_ERRNAME_MAX],")
    L.append("    pub datatype_name: [c_char; KWABI_ERRNAME_MAX],")
    L.append("    pub constraint_name: [c_char; KWABI_ERRNAME_MAX],")
    L.append("}")
    L.append("")
    L.append("impl KwabiError {")
    L.append("    /// Offset of a field, for the size guards the writer uses.")
    L.append("    pub fn new() -> Self { Self::default() }")
    L.append("}")
    L.append("")
    L.append("impl Default for KwabiError {")
    L.append("    fn default() -> Self {")
    L.append("        KwabiError {")
    L.append("            size: std::mem::size_of::<KwabiError>() as u32,")
    L.append("            sqlerrcode: 0,")
    L.append("            status: KWABI_OK,")
    L.append("            message: [0; KWABI_ERRMSG_MAX],")
    L.append("            detail: [0; KWABI_ERRDETAIL_MAX],")
    L.append("            hint: [0; KWABI_ERRHINT_MAX],")
    L.append("            schema_name: [0; KWABI_ERRNAME_MAX],")
    L.append("            table_name: [0; KWABI_ERRNAME_MAX],")
    L.append("            column_name: [0; KWABI_ERRNAME_MAX],")
    L.append("            datatype_name: [0; KWABI_ERRNAME_MAX],")
    L.append("            constraint_name: [0; KWABI_ERRNAME_MAX],")
    L.append("        }")
    L.append("    }")
    L.append("}")
    L.append("")
    L.append("/// A guarded body. Must not raise, and must not panic.")
    L.append("///")
    L.append("/// `extern \"C\"`, deliberately. A panic escaping it aborts the process at")
    L.append("/// the body's own boundary, which is the body's bug and is caught during")
    L.append("/// development. `C-unwind` was tried and is WORSE: it lets the panic")
    L.append("/// travel into the runtime's catch_unwind, where -- if the body came from")
    L.append("/// a separately built extension with its own Rust std -- it is a *foreign")
    L.append("/// exception* and aborts anyway, with a much less useful message. See")
    L.append("/// notes/error-firewall-design.md section 4.")
    L.append("/// A guarded body. Must not raise, and must not panic.")
    L.append("///")
    L.append("/// The second parameter is the caller's error channel: it is how a body")
    L.append("/// reports a STRUCTURED error rather than only a status. Added after the")
    L.append("/// first prototype, where `(void *arg)` alone left the body with no way")
    L.append("/// to reach the channel at all.")
    L.append("pub type KwabiBodyFn =")
    L.append("    unsafe extern \"C\" fn(*mut c_void, *mut KwabiError) -> c_int;")
    L.append("")
    # Executor hook bodies. Each returns a KwabiStatus (c_int) and takes the
    # `next` handle and error channel after its PostgreSQL arguments.
    L.append("/// Executor hook bodies. Must not raise or unwind; see kwabi.h.")
    L.append("pub type KwabiExecutorStartBody =")
    L.append("    unsafe extern \"C\" fn(*mut c_void, c_int, *mut c_void, *mut KwabiError, *mut c_void) -> c_int;")
    L.append("pub type KwabiExecutorRunBody =")
    L.append("    unsafe extern \"C\" fn(*mut c_void, c_int, u64, *mut c_void, *mut KwabiError, *mut c_void) -> c_int;")
    L.append("pub type KwabiExecutorFinishBody =")
    L.append("    unsafe extern \"C\" fn(*mut c_void, *mut c_void, *mut KwabiError, *mut c_void) -> c_int;")
    L.append("pub type KwabiExecutorEndBody =")
    L.append("    unsafe extern \"C\" fn(*mut c_void, *mut c_void, *mut KwabiError, *mut c_void) -> c_int;")
    L.append("")
    L.append("#[repr(C)]")
    L.append("pub struct KwabiV1 {")
    for rust_t, name, comment in fields:
        if comment:
            L.append(f"    /// {comment}")
        L.append(f"    pub {name}: {rust_t},")
    L.append("}")
    L.append("")
    L.append("impl KwabiV1 {")
    L.append("    /// Number of fields; asserted against the header by the harness.")
    L.append(f"    pub const FIELD_COUNT: usize = {len(fields)};")
    L.append("}")
    L.append("")
    L.append("/// Every slot starts null. An extension MUST test a slot before calling it;")
    L.append("/// a null slot is how this ABI says \"this runtime does not implement that")
    L.append("/// yet\". Slot presence and capability bits answer different questions -- see")
    L.append("/// the `capabilities` slot and the KWABI_CAP_* constants.")
    L.append("impl Default for KwabiV1 {")
    L.append("    fn default() -> Self {")
    L.append("        KwabiV1 {")
    for rust_t, name, comment in fields:
        if name == "version":
            L.append("            version: KWABI_VERSION,")
        else:
            L.append(f"            {name}: None,")
    L.append("        }")
    L.append("    }")
    L.append("}")
    L.append("")
    return "\n".join(L)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "kwabi.h"
    with open(path, "r", encoding="utf-8") as fh:
        header = strip_comments(fh.read())
    sys.stdout.write(emit(parse_struct(header)))


if __name__ == "__main__":
    main()
