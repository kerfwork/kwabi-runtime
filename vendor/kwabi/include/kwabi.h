/*
 * kwabi.h — kwabi Stable ABI for PostgreSQL
 *
 * This header defines the binary interface between kwabi extensions and the
 * kwabi runtime. Extensions compile against this header only; they do not
 * link against PostgreSQL symbols.
 *
 * The runtime passes a versioned function table to the extension at load
 * time. All PostgreSQL access goes through this table.
 *
 * Supported PostgreSQL versions: 16, 17, 18
 * kwabi ABI version: 1
 */

#ifndef KWABI_H
#define KWABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>   /* memset, in kwabi_error_init */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Version
 * ======================================================================== */

#define KWABI_VERSION_1 1

/* ========================================================================
 * Capabilities
 *
 * A bitset telling an extension what a runtime GUARANTEES, as distinct from
 * which slots exist.
 *
 * The distinction is the whole point. A NULL slot says "this runtime does not
 * implement that function". It cannot say "this runtime implements the
 * function, but with a weaker guarantee than you need". That gap is real: an
 * extension that needs a failed body to leave no partial work cannot learn
 * from a non-NULL `try_body` whether rollback is included or whether the slot
 * merely catches the error.
 *
 * RULE, and it is a hard one: a capability bit MUST NOT be redundant with
 * "slot X is non-NULL". If a bit can be derived by testing a slot, the bit is
 * noise and does not belong here. Every bit below describes a guarantee, a
 * behaviour, or a version-derived fact — never a slot's presence.
 *
 * The bitset is 64 bits, of which 5 are defined. The rest are reserved and
 * MUST be zero, so a future bit cannot collide with a value an older runtime
 * happened to produce.
 * ======================================================================== */

/* The table is minimally complete and every always-present slot is wired.
 * One bit to check instead of fifty. Any conforming runtime sets this. */
#define KWABI_CAP_CORE                     (1ULL << 0)

/* `KwabiError` carries the extended fields (detail, hint, schema/table/
 * column/datatype/constraint), and the size protocol is honoured on write.
 *
 * NOT derivable from a slot test: `error_get` is non-NULL whether or not the
 * runtime fills anything beyond `message`. An extension that wants to show a
 * DETAIL line needs to know which it is. */
#define KWABI_CAP_STRUCTURED_ERRORS        (1ULL << 1)

/* `try_body` contains BOTH a PostgreSQL ERROR and a Rust panic, and the
 * subtransaction undoes the body's partial work.
 *
 * NOT derivable: a runtime could implement `try_body` that catches the error
 * but leaves partial work committed — the exact hazard documented in
 * error-firewall-design.md. Slot presence cannot distinguish the two. */
#define KWABI_CAP_ERROR_FIREWALL           (1ULL << 2)

/* `memory_chunk_context` and `current_memory_context` are genuine: the values
 * they return can be compared to prove a pointer is real PostgreSQL memory
 * owned by the expected context. */
#define KWABI_CAP_MEMORY_INTROSPECTION     (1ULL << 3)

/* A failed body leaves NO partial work: writes it made before failing are
 * rolled back. Measured, not assumed — see the survivors=0 assertions in
 * runtime-skeleton/shim/try.sql.
 *
 * Separate from KWABI_CAP_ERROR_FIREWALL because a runtime could contain the
 * error without the subtransaction, and an extension that writes must know
 * which it is. This is the most consequential bit in the set. */
#define KWABI_CAP_ATOMIC_BODY              (1ULL << 4)

/* The SLRU slots (slru_create/read/write) are FUNCTIONAL, not merely present.
 *
 * NOT derivable from a slot test, and this is the clearest case in the set:
 * the slots are wired on EVERY runtime, preloaded or not, because a NULL slot
 * would say "not implemented" when the truth is "implemented, but needs a
 * preload". An extension that tests the slot learns nothing. This bit is the
 * only way to ask "is SLRU actually usable here?" without calling and
 * catching the error.
 *
 * The runtime sets it only when it was loaded via shared_preload_libraries
 * AND at least one SLRU was declared and initialised. See group_slru.c. */
#define KWABI_CAP_SLRU                     (1ULL << 5)

/* Every defined bit, for a runtime that supports the lot. */
#define KWABI_CAP_ALL \
    (KWABI_CAP_CORE | KWABI_CAP_STRUCTURED_ERRORS | KWABI_CAP_ERROR_FIREWALL | \
     KWABI_CAP_MEMORY_INTROSPECTION | KWABI_CAP_ATOMIC_BODY | KWABI_CAP_SLRU)
#define KWABI_VERSION KWABI_VERSION_1

/* PostgreSQL version numbers (from pg_config.h) */
#define PG_VERSION_NUM_16 160000
#define PG_VERSION_NUM_17 170000
#define PG_VERSION_NUM_18 180000

/* ========================================================================
 * Opaque handle types
 *
 * Extensions never access struct fields directly. All access goes through
 * accessor functions in the function table.
 * ======================================================================== */

typedef void *KwabiRelation;
typedef void *KwabiNode;
typedef void *KwabiValue;
typedef void *KwabiTableAm;
typedef void *KwabiEState;
typedef void *KwabiMemoryContext;
typedef void *KwabiPlannerInfo;
typedef void *KwabiLogicalDecodingCtx;
typedef void *KwabiReorderBuffer;
typedef void *KwabiFmgrInfo;
typedef void *KwabiSPIResult;
typedef void *KwabiSPIPlan;
typedef void *KwabiOutputPluginCallbacks;
typedef void *KwabiList;
typedef void *KwabiPlan;
typedef void *KwabiSlot;
typedef void *KwabiQueryDesc;
typedef void *KwabiIntoClause;
typedef void *KwabiExplainState;
typedef void *KwabiParamListInfo;
typedef void *KwabiQueryEnvironment;
typedef void *KwabiSnapshot;
typedef void *KwabiTriggerDesc;
typedef void *KwabiTrigger;

/* ========================================================================
 * Basic type aliases (matching PostgreSQL's types)
 *
 * Suppress this block with KWABI_NO_PG_TYPE_ALIASES when the translation unit
 * already includes postgres.h. The shim that binds this ABI to a specific
 * PostgreSQL version is exactly that case: it needs PostgreSQL's real
 * definitions of Oid, Datum, MemoryContext and friends, and redefining them
 * here would be an error.
 *
 * The kwabi handle typedefs above are unaffected: those names are kwabi's own
 * and never collide.
 * ======================================================================== */

#ifndef KWABI_NO_PG_TYPE_ALIASES

typedef uint32_t Oid;
typedef uint32_t TransactionId;
typedef uint32_t BlockNumber;
typedef uint16_t OffsetNumber;
typedef int32_t  int32;
typedef int64_t  int64;
typedef uint64_t uint64;
typedef int16_t  int16;
typedef uint8_t  uint8;
typedef int8_t   int8;
typedef float    float4;
typedef double   float8;
typedef uint32_t CmdType;
typedef uint32_t LOCKMODE;
typedef uint32_t LWLockMode;
typedef uintptr_t Datum;
typedef void*    HeapTuple;
typedef void*    TupleDesc;
typedef void*    TupleTableSlot;
typedef void*    TableScanDesc;
typedef void*    Buffer;
typedef void*    Page;
typedef void*    ItemPointer;
typedef void*    BulkInsertState;
typedef void*    StringInfo;
typedef void*    List;
typedef void*    Node;
typedef void*    Plan;
typedef void*    ScanKey;
typedef void*    VacuumParams;
typedef void*    BufferAccessStrategy;
typedef void*    Relation;
typedef void*    LWLock;
typedef void*    slock_t;
typedef void*    BackendId;
typedef void*    MemoryContext;

#endif /* KWABI_NO_PG_TYPE_ALIASES */

/* ========================================================================
 * Enums
 * ======================================================================== */

typedef enum {
    KWABI_NODE_UNKNOWN = 0,
    KWABI_NODE_QUERY,
    KWABI_NODE_PLANNED_STMT,
    KWABI_NODE_PLAN,
    KWABI_NODE_TARGET_ENTRY,
    KWABI_NODE_RTE,
    KWABI_NODE_SORT_GROUP_CLAUSE,
    KWABI_NODE_AGGREF,
    KWABI_NODE_WINDOW_FUNC,
    KWABI_NODE_VAR,
    KWABI_NODE_CONST,
    KWABI_NODE_PARAM,
    KWABI_NODE_OP_EXPR,
    KWABI_NODE_FUNC_EXPR,
    KWABI_NODE_DISTINCT_EXPR,
    KWABI_NODE_NULLIF_EXPR,
    KWABI_NODE_SCALAR_ARRAY_OP_EXPR,
    KWABI_NODE_BOOL_EXPR,
    KWABI_NODE_SUB_LINK,
    KWABI_NODE_SUB_PLAN,
    KWABI_NODE_ALTERNATIVE_SUB_PLAN,
    KWABI_NODE_FIELD_SELECT,
    KWABI_NODE_FIELD_STORE,
    KWABI_NODE_RELABEL_TYPE,
    KWABI_NODE_COERCE_VIA_IO,
    KWABI_NODE_ARRAY_COERCE_EXPR,
    KWABI_NODE_ROW_COMPARE_EXPR,
    KWABI_NODE_COALESCE_EXPR,
    KWABI_NODE_MIN_MAX_EXPR,
    KWABI_NODE_SQLVALUE_FUNCTION,
    KWABI_NODE_XML_EXPR,
    KWABI_NODE_NULL_TEST,
    KWABI_NODE_BOOLEAN_TEST,
    KWABI_NODE_CURRENT_OF_EXPR,
    KWABI_NODE_NEXT_VALUE_EXPR,
    KWABI_NODE_INFERENCE_ELEM,
    KWABI_NODE_TARGET_ENTRY_2,
    KWABI_NODE_JOIN_EXPR,
    KWABI_NODE_FROM_EXPR,
    KWABI_NODE_ON_CONFLICT_EXPR,
    KWABI_NODE_TYPE_NAME,
} KwabiNodeType;

typedef enum {
    KWABI_CMD_UNKNOWN = 0,
    KWABI_CMD_SELECT,
    KWABI_CMD_UPDATE,
    KWABI_CMD_INSERT,
    KWABI_CMD_DELETE,
    KWABI_CMD_UTILITY,
    KWABI_CMD_NOTHING,
} KwabiCmdType;

typedef enum {
    KWABI_LOCKMODE_NONE = 0,
    KWABI_LOCKMODE_SHARE,
    KWABI_LOCKMODE_EXCLUSIVE,
} KwabiLockMode;

typedef enum {
    KWABI_LWLOCKMODE_SHARE = 0,
    KWABI_LWLOCKMODE_EXCLUSIVE,
    KWABI_LWLOCKMODE_WAIT,
} KwabiLWLockMode;

/* ========================================================================
 * Callback types
 *
 * `bgworker_main_type` is PostgreSQL's own name for this callback (and its
 * signature is `void (*)(Datum)`). It is suppressed alongside the other PG
 * aliases for the same reason: when postgres.h is already included, the real
 * definition must win.
 * ======================================================================== */

#ifndef KWABI_NO_PG_TYPE_ALIASES
typedef void (*bgworker_main_type)(void *arg);
#endif
typedef void (*KwabiOutputPluginStartup)(void *callbacks);
typedef void (*KwabiOutputPluginShutdown)(void *callbacks);

/* ========================================================================
 * Error firewall types
 * ======================================================================== */

/*
 * Status codes. `KWABI_OK` is 0 so a plain integer check reads naturally.
 *
 * `KWABI_ERR_RAISED` means PostgreSQL raised inside the guarded body and the
 * subtransaction was rolled back. The body's work did not happen.
 *
 * `KWABI_ERR_PANICKED` means the body panicked. The panic was caught at the
 * runtime boundary and converted here; the backend is alive and the
 * subtransaction was rolled back.
 *
 * `KWABI_ERR_BODY_RAISED` means the body called `raise_error`, which the
 * contract forbids. It is reported rather than trapped because the jump has
 * already happened by then and there is nothing safe left to do.
 */
typedef enum {
    KWABI_OK = 0,
    KWABI_ERR_RAISED = 1,
    KWABI_ERR_PANICKED = 2,
    KWABI_ERR_BODY_RAISED = 3,
    KWABI_ERR_BAD_ARG = 4,
} KwabiStatus;

/*
 * The error captured from a failed `try_body`, or reported by an extension.
 *
 * # Why there are two structs
 *
 * `KwabiErrorV1` is what shipped. `KwabiError` is the current one. Both exist
 * because an extension built against an older header must keep working against
 * a newer runtime — that is the product, not a nicety.
 *
 * `try_body` and `error_get` take a `KwabiError *`, so a v1 extension passing
 * a v1 struct to a v2 runtime is a *type mismatch the compiler cannot see*:
 * both are `KwabiError *` at the ABI level. The runtime would write the larger
 * v2 struct into the smaller v1 buffer and overflow it.
 *
 * The `size` field is what prevents that. It is first in both structs, so a
 * writer can compare it against what it knows and refuse to write past the
 * caller's end. Writers MUST check it; a writer that assumes v2 is a bug.
 *
 * # Fields
 *
 * All strings are fixed-size arrays rather than pointers, so the struct carries
 * no ownership question: it can cross the boundary by value, be copied freely,
 * and be zeroed without leaking. The cost is that a long message is truncated
 * rather than allocated, which is the right trade for diagnostic text.
 *
 * `sqlerrcode` is a PostgreSQL SQLSTATE when PostgreSQL raised, and 0 when an
 * extension reported its own error. Extensions that want their own code should
 * use the SQLSTATE range reserved for applications (see `KWABI_SQLSTATE_BASE`).
 */

/* Sizes are part of the ABI: changing one changes the struct's layout. */
#define KWABI_ERRMSG_MAX    512   /* primary message */
#define KWABI_ERRDETAIL_MAX 512   /* detail */
#define KWABI_ERRHINT_MAX   384   /* hint */
#define KWABI_ERRNAME_MAX   128   /* one object name (schema, table, column, ...) */

/*
 * Applications may raise SQLSTATEs in the "application-defined" classes.
 * PostgreSQL reserves class 70 (ERRCODE_APPLICATION_ERROR) for exactly this;
 * using it keeps kwabi errors distinguishable from core errors in logs and in
 * client-side error handling.
 */
#define KWABI_SQLSTATE_APPLICATION 0x50000   /* class 50, unused by core */

typedef struct KwabiError {
    /* --- must be first in every version of this struct --- */
    uint32_t size;          /* sizeof(KwabiError) as the caller compiled it */

    /* --- core fields --- */
    int32  sqlerrcode;      /* SQLSTATE, or 0 for an extension-reported error */
    int32  status;          /* a KwabiStatus value */

    char message[KWABI_ERRMSG_MAX];   /* primary message, NUL-terminated */
    char detail[KWABI_ERRDETAIL_MAX]; /* optional detail, "" if none */
    char hint[KWABI_ERRHINT_MAX];     /* optional hint, "" if none */

    /*
     * Object names. These exist so an application can handle an error without
     * parsing the message text — the same reason PostgreSQL's own errtable()
     * family exists. Empty string means "not applicable".
     */
    char schema_name[KWABI_ERRNAME_MAX];
    char table_name[KWABI_ERRNAME_MAX];
    char column_name[KWABI_ERRNAME_MAX];
    char datatype_name[KWABI_ERRNAME_MAX];
    char constraint_name[KWABI_ERRNAME_MAX];

} KwabiError;

/* The previous layout, kept so v1 extensions can still be written against it.
 * New code should use KwabiError. */
typedef struct KwabiErrorV1 {
    uint32_t size;
    int32    sqlerrcode;
    int32    status;
    char     message[512];
} KwabiErrorV1;

/*
 * Fill `err` with a value this runtime produces, respecting the caller's size.
 *
 * `err` may be a v1 or a v2 struct; only the fields present in `err->size` are
 * written. This is the function every writer must go through rather than
 * writing fields directly.
 *
 * Returns the number of bytes written, or 0 if `err` was too small to hold
 * even the core fields.
 */
static inline uint32_t
kwabi_error_set_size(KwabiError *err)
{
    if (err == NULL)
        return 0;
    err->size = (uint32_t) sizeof(KwabiError);
    return err->size;
}

/*
 * Initialise an error struct before handing it to the ABI.
 *
 * CALL THIS. The size protocol depends on `err->size` being accurate, and an
 * uninitialised stack struct has garbage there — every size guard then fails
 * and the caller silently receives no error information. This was found the
 * hard way: a test declared `KwabiError err;` and got binary noise back.
 *
 * Zeroing as well as sizing means an "empty" error reads as empty strings
 * rather than whatever was on the stack.
 *
 *     KwabiError err;
 *     kwabi_error_init(&err);
 *     kwabi_try_body(body, arg, &err);
 */
static inline void
kwabi_error_init(KwabiError *err)
{
    if (err == NULL)
        return;
    memset(err, 0, sizeof(*err));
    err->size = (uint32_t) sizeof(KwabiError);
}

/*
 * A guarded body.
 *
 * THE CONTRACT (this is the answer to the open question in §8):
 *   - The body is `extern "C"`, so it may be written in any language.
 *   - The body receives the caller's `KwabiError *` as its second parameter,
 *     so it can report a STRUCTURED error rather than only a status. This was
 *     added after the first prototype: with `(void *arg)` alone the body had
 *     no way to reach the error channel, so an extension's message could not
 *     be delivered at all.
 *   - The body MUST NOT raise a PostgreSQL error. Raising would longjmp, and
 *     the body is reached from Rust, so the jump would cross Rust frames —
 *     the exact thing the firewall exists to prevent.
 *   - The body signals failure by RETURNING a non-OK KwabiStatus.
 *   - The body may panic (Rust) or be interrupted by PostgreSQL raising from
 *     inside a PostgreSQL call it makes. Both are contained: the panic by
 *     catch_unwind at the runtime boundary, the raise by PG_TRY in the shim.
 *
 * Why the body is `extern "C"` and not `extern "C-unwind"`: with `C-unwind`,
 * a panic could legally unwind out of the body and across the C shim, which is
 * precisely the behaviour being forbidden. `extern "C"` makes an escaping
 * panic abort at the body's own boundary — which is a bug in the body, caught
 * during development rather than shipped.
 */
typedef KwabiStatus (*KwabiBodyFn)(void *arg, KwabiError *out);

/* ========================================================================
 * Error channel helpers
 *
 * Every writer of a KwabiError goes through these, because the caller's
 * struct may be smaller than the writer's. Writing fields directly is the
 * buffer-overflow bug described above.
 * ======================================================================== */

/*
 * Copy at most `dstlen-1` bytes and NUL-terminate. Always leaves a valid
 * C string, even when truncating.
 */
static inline void
kwabi_strlcpy(char *dst, const char *src, uint32_t dstlen)
{
    uint32_t i = 0;
    if (dst == NULL || dstlen == 0)
        return;
    if (src != NULL) {
        for (; i + 1 < dstlen && src[i] != '\0'; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

/*
 * Does the caller's struct extend far enough to contain `offsetof(field) +
 * sizeof(field)`? Every optional field write is guarded by this.
 */
#define KWABI_ERR_HAS(err, field) \
    ((err) != NULL && (err)->size >= (uint32_t) (offsetof(KwabiError, field) + sizeof((err)->field)))

/*
 * Set the core fields. Safe for both v1 and v2 structs: `size`, `sqlerrcode`,
 * `status` and `message` are at identical offsets in both.
 *
 * Returns false if the caller's struct is too small to hold even these, in
 * which case nothing was written and the caller has no error information.
 */
static inline bool
kwabi_error_set_core(KwabiError *err, int sqlerrcode, int status, const char *message)
{
    if (err == NULL || err->size < offsetof(KwabiError, message) + 32)
        return false;

    err->sqlerrcode = sqlerrcode;
    err->status = status;
    kwabi_strlcpy(err->message, message, KWABI_ERRMSG_MAX);
    return true;
}

/*
 * Set the optional fields, skipping any the caller's struct does not have.
 *
 * A v1 caller gets the core fields and silently misses these — which is the
 * correct degradation: better than an overflow, and better than refusing to
 * report an error at all.
 */
static inline void
kwabi_error_set_detail(KwabiError *err, const char *detail, const char *hint)
{
    if (err == NULL)
        return;
    if (KWABI_ERR_HAS(err, detail))
        kwabi_strlcpy(err->detail, detail, KWABI_ERRDETAIL_MAX);
    if (KWABI_ERR_HAS(err, hint))
        kwabi_strlcpy(err->hint, hint, KWABI_ERRHINT_MAX);
}

static inline void
kwabi_error_set_object(KwabiError *err,
                       const char *schema, const char *table, const char *column,
                       const char *datatype, const char *constraint)
{
    if (err == NULL)
        return;
    if (KWABI_ERR_HAS(err, schema_name))
        kwabi_strlcpy(err->schema_name, schema, KWABI_ERRNAME_MAX);
    if (KWABI_ERR_HAS(err, table_name))
        kwabi_strlcpy(err->table_name, table, KWABI_ERRNAME_MAX);
    if (KWABI_ERR_HAS(err, column_name))
        kwabi_strlcpy(err->column_name, column, KWABI_ERRNAME_MAX);
    if (KWABI_ERR_HAS(err, datatype_name))
        kwabi_strlcpy(err->datatype_name, datatype, KWABI_ERRNAME_MAX);
    if (KWABI_ERR_HAS(err, constraint_name))
        kwabi_strlcpy(err->constraint_name, constraint, KWABI_ERRNAME_MAX);
}

/* ========================================================================
 * The function table
 *
 * This is the interface. Extensions receive a pointer to this struct at
 * load time. All PostgreSQL access goes through it.
 *
 * New functions are appended in future versions. Never reorder or remove.
 * ======================================================================== */

typedef struct KwabiV1 {
    uint32_t version;  /* KWABI_VERSION_1 */

    /* ---- Function calls (fmgr) ----
     *
     * These five slots are wired by the per-version SHIM, not by the runtime,
     * even though the header groups them with the memory slots. The reason is
     * the error firewall, not version churn:
     *
     *   `fmgr_info` can `ereport(ERROR)` — `fmgr_info_cxt_security` does
     *   `elog(ERROR, "cache lookup failed for function %u")` — and a call to a
     *   function that raises will `longjmp` out of the call. A Rust wrapper
     *   would put a Rust frame between the raise and the `PG_TRY`, which is
     *   exactly what the firewall forbids. So these live in the shim, beside
     *   `try_body`.
     *
     * The call path itself needs NO version guard: `FunctionCallInfoBaseData`,
     * `FmgrInfo` and `FunctionCallInvoke` are byte-identical on PostgreSQL
     * 16.15 / 17.11 / 18.6 (verified). The 17→18 `fmgr.h` churn is
     * `PG_MODULE_MAGIC_EXT` and `dfmgr`, not call machinery.
     *
     * # Why these signatures changed before v1 froze
     *
     * They were declared as `Datum (*call_functionN)(KwabiFmgrInfo, Datum ...)`
     * — a bare `Datum` return. That shape cannot express SQL NULL: `Datum` is
     * an integer with no null channel, and PostgreSQL signals a NULL result
     * out-of-band through `fcinfo->isnull`. Returning a bare `Datum` would make
     * a NULL result indistinguishable from 0, which is a silent wrong answer.
     *
     * `abi-principles.md` §2 is explicit that "structs can grow later behind
     * their size field; signatures cannot change later at all", so the fix must
     * happen now, while v1 is still open. Nothing has ever been built against
     * the old shape (the slots were never wired), and `abi-v1-freeze` is blocked
     * on this very step. So the signatures are corrected here rather than
     * shipped wrong.
     *
     * # Contract
     *
     *   - `fmgr_info` returns NULL if the lookup failed. The reason is then
     *     readable through `error_get`. The `FmgrInfo` is allocated in the
     *     CURRENT memory context, so it lives as long as that context does;
     *     switch context first to give it a different lifetime.
     *   - Every `call_function*` returns `KWABI_OK` on success and fills
     *     `*isnull` and `*result`. `*isnull == true` means the function
     *     returned SQL NULL, and `*result` is then meaningless — NULL is a
     *     normal outcome, NOT an error.
     *   - `isnull` and `result` are required (non-NULL); a NULL one is
     *     `KWABI_ERR_BAD_ARG`.
     *   - On a non-OK status the call did not complete; `error_get` carries the
     *     PostgreSQL error (SQLSTATE, message, detail, hint). The subtransaction
     *     rule does not apply here: nothing is swallowed, the caller sees a
     *     status and decides.
     *   - `nargs` must be 0..FUNC_MAX_ARGS. `argnulls` may be NULL only when
     *     `nargs == 0`; otherwise it is an array of `nargs` flags, one per arg.
     *
     * A function marked `strict` is NOT short-circuited here. `FunctionCallInvoke`
     * does not check `fn_strict`; strictness is honoured by callers that go
     * through `InputFunctionCall` or the executor, and by the function itself.
     * Passing a NULL to a strict function therefore calls it. Use
     * `api->fmgr_info` + the strict flag if you need the short-circuit — or,
     * more simply, check `argnulls` yourself.
     */
    KwabiFmgrInfo (*fmgr_info)(Oid fn_oid);
    KwabiStatus (*call_function)(KwabiFmgrInfo info, int nargs, Datum *args,
                                 const bool *argnulls, bool *isnull, Datum *result);
    KwabiStatus (*call_function1)(KwabiFmgrInfo info, Datum arg1,
                                  bool *isnull, Datum *result);
    KwabiStatus (*call_function2)(KwabiFmgrInfo info, Datum arg1, Datum arg2,
                                  bool *isnull, Datum *result);
    KwabiStatus (*call_function3)(KwabiFmgrInfo info, Datum arg1, Datum arg2, Datum arg3,
                                  bool *isnull, Datum *result);


    /* ---- SPI ---- */
    KwabiSPIResult (*spi_execute)(const char *sql, bool read_only, int tcount);
    KwabiSPIResult (*spi_execute_plan)(KwabiSPIPlan plan, Datum *values, const char *nulls, bool read_only, int tcount);
    /* A plan outlives the SPI connection that made it, until spi_free_plan. */
    KwabiSPIPlan (*spi_prepare)(const char *sql, int nargs, Oid *argtypes);
    void (*spi_free_plan)(KwabiSPIPlan plan);
    void (*spi_free_result)(KwabiSPIResult result);
    int (*spi_result_ntuples)(KwabiSPIResult result);
    Datum (*spi_result_get_value)(KwabiSPIResult result, int tupno, int attno);

    /* ---- Type system ---- */
    Datum (*type_input)(Oid type_oid, const char *input, int32 typmod);
    char *(*type_output)(Oid type_oid, Datum value);
    Datum (*type_recv)(Oid type_oid, StringInfo buf);
    void (*type_send)(Oid type_oid, Datum value, StringInfo buf);
    Oid (*type_element_type)(Oid type_oid);
    int16 (*type_length)(Oid type_oid);
    bool (*type_is_array)(Oid type_oid);
    bool (*type_is_composite)(Oid type_oid);
    Oid (*type_base_type)(Oid type_oid);

    /* ---- Parser ---- */
    KwabiNode (*parse_expr)(const char *sql, Oid *argtypes, int nargs);
    KwabiNode (*parse_stmt)(const char *sql);
    KwabiNode (*parse_type)(const char *type_name);
    void (*free_node)(KwabiNode node);
    Oid (*oper_left_type)(Oid oper_oid);
    Oid (*oper_right_type)(Oid oper_oid);
    Oid (*oper_result_type)(Oid oper_oid);
    bool (*oper_is_commutative)(Oid oper_oid);

    /* ---- Commands ---- */
    Oid (*extension_oid)(const char *extname);
    bool (*extension_installed)(const char *extname);
    const char *(*extension_version)(const char *extname);
    int64 (*sequence_nextval)(Oid seq_oid);
    int64 (*sequence_currval)(Oid seq_oid);
    int64 (*sequence_setval)(Oid seq_oid, int64 value);

    /* ---- Replication ---- */
    KwabiLogicalDecodingCtx (*logical_decoding_begin)(const char *slot_name, int64 start_lsn);
    void (*logical_decoding_end)(KwabiLogicalDecodingCtx ctx);
    bool (*logical_decoding_read)(KwabiLogicalDecodingCtx ctx, int64 *lsn, StringInfo data);
    void (*output_plugin_startup)(KwabiOutputPluginCallbacks callbacks);
    void (*output_plugin_shutdown)(KwabiOutputPluginCallbacks callbacks);

    /* ---- Background workers ---- */
    Oid (*bgworker_register)(const char *name, bgworker_main_type main, void *arg);
    void (*bgworker_terminate)(Oid bgw_oid);
    bool (*bgworker_is_running)(Oid bgw_oid);

    /* ---- Storage primitives ---- */
    BlockNumber (*block_get_number)(ItemPointer pointer);
    OffsetNumber (*block_get_offset)(ItemPointer pointer);
    bool (*block_is_valid)(ItemPointer pointer);
    void (*slru_create)(const char *name, int nblocks, int nslots);
    void (*slru_read)(const char *name, int64 pageno, void *data);
    void (*slru_write)(const char *name, int64 pageno, const void *data);

    /* ---- Value nodes ---- */
    bool (*value_is_null)(KwabiValue value);
    Datum (*value_get_datum)(KwabiValue value);
    Oid (*value_get_type)(KwabiValue value);
    int32 (*value_get_typmod)(KwabiValue value);

    /* ---- Memory contexts ---- */
    void *(*palloc)(size_t size);
    void *(*palloc0)(size_t size);
    void *(*repalloc)(void *pointer, size_t size);
    void (*pfree)(void *pointer);
    KwabiMemoryContext (*memory_context_current)(void);
    KwabiMemoryContext (*memory_context_switch_to)(KwabiMemoryContext context);
    void (*memory_context_reset)(KwabiMemoryContext context);
    void (*memory_context_delete)(KwabiMemoryContext context);

    /* ---- Error handling ---- */
    void (*ereport)(int errcode, const char *fmt, ...);
    void (*elog)(int elevel, const char *fmt, ...);
    const char *(*error_message)(void);
    int (*error_code)(void);
    void (*error_clear)(void);

    /* ---- Relation cache ---- */
    KwabiRelation (*relation_open)(Oid relid, KwabiLockMode lockmode);
    void (*relation_close)(KwabiRelation rel, KwabiLockMode lockmode);
    Oid (*relation_id)(KwabiRelation rel);
    const char *(*relation_name)(KwabiRelation rel);
    Oid (*relation_namespace)(KwabiRelation rel);
    TupleDesc (*relation_tupledesc)(KwabiRelation rel);

    /* ---- System cache ---- */
    Oid (*syscache_get_oid)(const char *cache_name, const char *attname, Datum key);
    HeapTuple (*syscache_get_tuple)(const char *cache_name, Datum key);
    void (*syscache_free_tuple)(HeapTuple tuple);

    /* ---- Optimizer ---- */
    KwabiPlannerInfo (*planner_info)(KwabiNode parse, int cursorOptions, KwabiParamListInfo boundParams);
    void (*free_planner_info)(KwabiPlannerInfo info);
    double (*planner_estimate_rows)(KwabiPlannerInfo info, KwabiList quals);
    double (*planner_estimate_cost)(KwabiPlannerInfo info, KwabiList quals);

    /* ---- Transactions ----
     *
     * One accessor, deliberately. An extension reached from a SQL-callable
     * function is ALREADY inside a transaction, so it cannot start, commit or
     * abort one: the command-level API raises "unexpected state STARTED", and
     * the block-level API (BeginTransactionBlock/EndTransactionBlock) is the
     * tcop command-loop state machine that the BEGIN/COMMIT statements drive --
     * calling EndTransactionBlock() from inside a command is a FATAL that drops
     * the connection, not a feature. Real transaction boundaries inside a
     * routine belong to the PL layer (a procedure's COMMIT); partial-rollback
     * atomicity is already the `try_body` slot's job. So there is no
     * start/commit/abort surface to expose, and a NULL slot would be a promise
     * this ABI can never keep. Only the identity accessor is meaningful here.
     *
     * Returns the current top-level transaction id, or 0 when the current
     * transaction has not been assigned one yet (read-only, or no write so
     * far). Non-allocating on purpose: asking for the id must not force an XID
     * into existence. 0 is not an error -- it is the honest answer. */
    int64 (*transaction_get_current_xid)(void);

    /* ---- Storage ---- */
    void *(*shmem_alloc)(size_t size);
    void (*shmem_free)(void *pointer);
    void *(*shmem_get)(const char *name, size_t size);
    void (*lock_acquire)(LWLock lock, KwabiLWLockMode mode);
    void (*lock_release)(LWLock lock);
    bool (*lock_held_by_me)(LWLock lock);
    void (*spin_acquire)(slock_t lock);
    void (*spin_release)(slock_t lock);

    /* ---- Postmaster ---- */
    bool (*autovacuum_is_running)(void);
    int (*autovacuum_naptime)(void);
    void (*syslogger_log)(const char *msg);

    /* ---- WAL replication ---- */
    void (*walsender_send)(const char *data, int len);
    int (*walsender_receive)(char *buf, int len);
    bool (*walsender_is_connected)(void);

    /* ---- Commands (defrem) ---- */
    void (*defrem_create)(const char *name, const char *type, const char *value);
    void (*defrem_alter)(const char *name, const char *value);
    void (*defrem_drop)(const char *name);

    /* ---- Node trees ---- */
    KwabiNodeType (*node_type)(KwabiNode node);
    const char *(*node_type_name)(KwabiNode node);
    KwabiList (*node_get_list)(KwabiNode node);
    int (*node_list_length)(KwabiNode node);
    KwabiNode (*node_list_get)(KwabiNode node, int index);
    KwabiCmdType (*query_command_type)(KwabiNode query);
    KwabiList (*query_rtable)(KwabiNode query);
    KwabiList (*query_target_list)(KwabiNode query);
    KwabiList (*query_returning_list)(KwabiNode query);
    KwabiNode (*query_jointree)(KwabiNode query);
    KwabiList (*query_group_clause)(KwabiNode query);
    KwabiList (*query_sort_clause)(KwabiNode query);
    KwabiNode (*query_limit_offset)(KwabiNode query);
    KwabiNode (*query_limit_count)(KwabiNode query);
    bool (*query_has_for_update)(KwabiNode query);
    bool (*query_has_row_security)(KwabiNode query);
    KwabiPlan (*planned_stmt_plan_tree)(KwabiNode stmt);
    KwabiList (*planned_stmt_rtable)(KwabiNode stmt);
    KwabiList (*planned_stmt_result_relations)(KwabiNode stmt);
    bool (*planned_stmt_has_returning)(KwabiNode stmt);
    bool (*planned_stmt_has_modifying_cte)(KwabiNode stmt);
    bool (*planned_stmt_is_utility)(KwabiNode stmt);

    /* ---- Tuples ---- */
    int (*tuple_natts)(TupleDesc tupdesc);
    Oid (*tuple_typeid)(TupleDesc tupdesc, int attno);
    int32 (*tuple_typmod)(TupleDesc tupdesc, int attno);
    const char *(*tuple_attname)(TupleDesc tupdesc, int attno);
    bool (*tuple_attisdropped)(TupleDesc tupdesc, int attno);
    int (*tuple_attnum)(TupleDesc tupdesc, const char *attname);
    Datum (*heap_tuple_getattr)(HeapTuple tuple, int attno, TupleDesc tupdesc, bool *isnull);
    HeapTuple (*heap_tuple_setattr)(HeapTuple tuple, int attno, Datum value, TupleDesc tupdesc);
    Oid (*heap_tuple_tableoid)(HeapTuple tuple);
    ItemPointer (*heap_tuple_tid)(HeapTuple tuple);
    bool (*slot_isnull)(KwabiSlot slot, int attno);
    Datum (*slot_getattr)(KwabiSlot slot, int attno, bool *isnull);
    TupleDesc (*slot_tupledesc)(KwabiSlot slot);

    /* ---- Table AM ---- */
    /* A table AM handle is a Relation, opened with relation_open. table_am_get
     * returns that relation's access method routine. */
    KwabiTableAm (*table_am_get)(KwabiRelation rel);
    TableScanDesc (*table_am_beginscan)(KwabiRelation rel, KwabiSnapshot snapshot, int nkeys, ScanKey key);
    void (*table_am_endscan)(TableScanDesc scan);
    bool (*table_am_getnext)(TableScanDesc scan, KwabiSlot slot);
    void (*table_am_insert)(KwabiRelation rel, KwabiSlot slot, int options, BulkInsertState bistate);
    void (*table_am_update)(KwabiRelation rel, KwabiSlot slot, int options);
    void (*table_am_delete)(KwabiRelation rel, KwabiSlot slot, int options);

    /* ---- Executor ---- */
    KwabiEState (*executor_start)(KwabiQueryDesc queryDesc, int eflags);
    void (*executor_run)(KwabiEState estate, int direction, long count, bool execute_once);
    void (*executor_finish)(KwabiEState estate);
    void (*executor_end)(KwabiEState estate);
    KwabiSlot (*executor_getnext)(KwabiEState estate);

    /* ---- Buffer manager ---- */
    Buffer (*buffer_get)(Relation rel, BlockNumber blocknum);
    void (*buffer_release)(Buffer buffer);
    Page (*buffer_get_page)(Buffer buffer);
    void (*buffer_mark_dirty)(Buffer buffer);

    /* ---- Locks ---- */
    void (*lwlock_acquire)(LWLock lock, KwabiLWLockMode mode);
    void (*lwlock_release)(LWLock lock);
    bool (*lwlock_held_by_me)(LWLock lock);
    bool (*lwlock_cond_acquire)(LWLock lock, KwabiLWLockMode mode);
    void (*spinlock_acquire)(slock_t lock);
    void (*spinlock_release)(slock_t lock);
    bool (*spinlock_held_by_me)(slock_t lock);

    /* ---- GUC ---- */
    int (*guc_get_int)(const char *name);
    const char *(*guc_get_string)(const char *name);
    bool (*guc_get_bool)(const char *name);
    double (*guc_get_float)(const char *name);
    void (*guc_set_int)(const char *name, int value);
    void (*guc_set_string)(const char *name, const char *value);
    void (*guc_set_bool)(const char *name, bool value);
    void (*guc_set_float)(const char *name, double value);

    /* ---- Explain ----
     * An ExplainState is created by explain_state_new and freed by
     * explain_state_free. The shim gives each one its own memory context, so
     * free releases all of it. explain_state_text is valid until the state is
     * freed or explain_query runs on it again.
     *
     * format is an int (not an enum, which has implementation-defined width
     * at the ABI): 0 = text, 1 = xml, 2 = json, 3 = yaml.
     *
     * A new state starts from the C defaults of ExplainState, not the SQL defaults:
     * on PG 18 SQL EXPLAIN ANALYZE turns BUFFERS on, the ABI state does not. Set
     * every option the caller depends on.
     *
     * explain_state_set_option takes a PostgreSQL EXPLAIN option name (verbose,
     * costs, buffers, wal, timing, summary, memory, settings, generic, analyze).
     * An unknown name raises. The version-specific options raise on majors that
     * do not have them.
     *
     * explain_query frames and runs the plan in queryDesc as SQL EXPLAIN does:
     * XML, JSON and YAML output is a complete document. With analyze set, the
     * plan runs, as it does for SQL EXPLAIN ANALYZE. */
    void (*explain_query)(KwabiQueryDesc queryDesc, KwabiIntoClause into, KwabiExplainState es, const char *queryString, KwabiParamListInfo params, KwabiQueryEnvironment queryEnv);
    KwabiExplainState (*explain_state_new)(void);
    void (*explain_state_set_option)(KwabiExplainState es, const char *name, bool value);
    void (*explain_state_set_format)(KwabiExplainState es, int format);
    const char *(*explain_state_text)(KwabiExplainState es);
    void (*explain_state_free)(KwabiExplainState es);
    const char *(*explain_get_index_name)(Oid indexOid);

    /* ---- Vacuum ---- */
    void (*vacuum_rel)(Relation rel, VacuumParams params, BufferAccessStrategy bstrategy);
    void (*vacuum_analyze_rel)(Relation rel, VacuumParams params, BufferAccessStrategy bstrategy);

    /* ---- Triggers ---- */
    KwabiTriggerDesc (*trigger_desc)(Oid relid);
    int (*trigger_count)(KwabiTriggerDesc desc);
    KwabiTrigger (*trigger_get)(KwabiTriggerDesc desc, int index);

    /* ---- Replication internals ---- */
    int64 (*reorderbuffer_get_lsn)(KwabiReorderBuffer rb);
    int64 (*reorderbuffer_get_xid)(KwabiReorderBuffer rb, TransactionId xid);
    int (*reorderbuffer_get_changes)(KwabiReorderBuffer rb, TransactionId xid);
    /* Replication slots are found by name; PostgreSQL gives slots no OID. Each
     * raises if no slot has that name. */
    int64 (*slot_get_lsn)(const char *slot_name);
    int64 (*slot_get_catalog_xmin)(const char *slot_name);
    bool (*slot_is_active)(const char *slot_name);

    /* ---- Postmaster ---- */
    bool (*postmaster_is_alive)(void);
    int (*postmaster_get_child_pid)(BackendId backend_id);

    /* ---- Item pointers ---- */
    BlockNumber (*itempointer_get_block_number)(ItemPointer pointer);
    OffsetNumber (*itempointer_get_offset_number)(ItemPointer pointer);
    bool (*itempointer_is_valid)(ItemPointer pointer);

    /* ---- Relations ---- */
    Oid (*rel_id)(KwabiRelation rel);
    const char *(*rel_name)(KwabiRelation rel);
    Oid (*rel_namespace)(KwabiRelation rel);
    char (*rel_relkind)(KwabiRelation rel);
    Oid (*rel_relam)(KwabiRelation rel);
    TupleDesc (*rel_tupledesc)(KwabiRelation rel);
    KwabiList (*rel_index_list)(KwabiRelation rel);

    /* ---- String info ---- */
    void (*stringinfo_init)(StringInfo str);
    void (*stringinfo_reset)(StringInfo str);
    void (*stringinfo_append)(StringInfo str, const char *data);
    void (*stringinfo_append_char)(StringInfo str, char c);
    void (*stringinfo_append_int)(StringInfo str, int64 value);
    const char *(*stringinfo_data)(StringInfo str);
    int (*stringinfo_len)(StringInfo str);

    /* ---- Memory introspection and error raising (appended, still v1) ----
     *
     * These three are installed by the per-version C shim rather than by the
     * runtime's Rust core. Two reasons:
     *
     *  - `memory_chunk_context` and `current_memory_context` let an extension
     *    prove that memory it got through `palloc` is genuine PostgreSQL
     *    memory, by comparing the chunk's owning context against the
     *    backend's current one.
     *  - `raise_error` raises a real PostgreSQL ERROR, which `longjmp`s. It
     *    must not be entered from a Rust frame, so the shim owns it.
     *
     * Appending is the only change this ABI permits; no earlier slot moved.
     */
    KwabiMemoryContext (*memory_chunk_context)(void *pointer);
    KwabiMemoryContext (*current_memory_context)(void);
    void (*raise_error)(int sqlerrcode, const char *msg);

    /* ---- Error firewall: catching (appended, still v1) ----
     *
     * `raise_error` above is the *outward* direction and it longjmps. That
     * makes it unsafe to call from extension code: the jump would cross the
     * extension's own Rust frames. It is only safe when the caller is C with
     * no Rust below it — i.e. from shim-authored SQL functions.
     *
     * `try_body` is the safe direction. The extension supplies a body; the
     * shim runs it inside PG_TRY and inside an internal subtransaction, and
     * returns a status instead of unwinding. The body must not raise; it
     * signals failure by returning non-OK. See
     * notes/error-firewall-design.md §2.
     */
    KwabiStatus (*try_body)(KwabiBodyFn body, void *arg, KwabiError *out);
    void (*error_get)(KwabiError *out);

    /* ---- Capabilities (appended, still v1) ----
     *
     * A FUNCTION, not a struct field, for three reasons:
     *
     *  1. It preserves this table's invariant that every field is exactly one
     *     pointer wide. The harness asserts `size == FIELD_COUNT * 8`, and a
     *     `uint64_t` field would satisfy that arithmetic while quietly making
     *     the assertion mean something weaker.
     *  2. It is appended, so nothing before it moves. Inserting a field after
     *     `version` would have shifted all 200+ following slots.
     *  3. It lets the answer be computed rather than declared — the runtime
     *     derives it from what it actually wired and the PostgreSQL major
     *     version, so it cannot drift from the truth.
     *
     * BOOTSTRAP: on a runtime older than this slot, `capabilities` is NULL.
     * That is not a failure — NULL means "no capability query; fall back to
     * testing slots directly", which is what every extension did before this
     * existed. An extension should therefore treat a NULL here as
     * KWABI_CAP_CORE-only, not as "nothing works".
     */
    uint64_t (*capabilities)(void);

    /* ---- Memory context creation (appended, still v1) ----
     *
     * Creates a new memory context under the CURRENT context, with the
     * given name. A name is required, not optional — PostgreSQL's own
     * AllocSetContextCreate takes one, and anonymous contexts are a
     * debugging dead end in a long-lived backend.
     *
     * AllocSetContextCreate is a macro, not a function, so it cannot be
     * forwarded across the ABI — the shim expands it. This slot is
     * therefore installed by the shim, not by the runtime.
     */
    KwabiMemoryContext (*memory_context_create)(const char *name);

} KwabiV1;

/* ========================================================================
 * Extension entry point
 *
 * The runtime calls this function at load time, passing the function table.
 * The extension stores the table and returns true on success.
 *
 * Returns: true if the extension loaded successfully, false otherwise.
 * ======================================================================== */

bool kwabi_ext_init(const KwabiV1 *api);

/* ========================================================================
 * Convenience macros
 * ======================================================================== */

#define KWABI_CHECK_VERSION(api, ver) ((api)->version >= (ver))

#define KWABI_PALLOC(api, sz)        (api)->palloc((sz))
#define KWABI_PFREE(api, ptr)        (api)->pfree((ptr))
#define KWABI_SPI_EXEC(api, sql)     (api)->spi_execute((sql), false, 0)
#define KWABI_REL_OPEN(api, oid)     (api)->relation_open((oid), KWABI_LOCKMODE_SHARE)
#define KWABI_REL_CLOSE(api, rel)    (api)->relation_close((rel), KWABI_LOCKMODE_SHARE)

#ifdef __cplusplus
}
#endif

#endif /* KWABI_H */
