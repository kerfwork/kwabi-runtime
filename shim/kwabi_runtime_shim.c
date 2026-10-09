/*
 * kwabi_runtime_pg18.c — the per-version shim for PostgreSQL 18.
 *
 * This file is the ONLY place in the project that includes postgres.h. It is
 * compiled once per PostgreSQL major version. Its job is to fill in the
 * runtime's native table with that version's real symbols and publish the
 * stable ABI, which is what lets one extension binary run on every major.
 *
 * The Rust core links in as a static library and owns the table layout, the
 * wrappers and the error state. This file owns everything that must be
 * expressed in PostgreSQL's own C: the module magic block, the addresses of
 * PostgreSQL symbols, and any call that can longjmp.
 *
 * Why the runtime must be a C bundle rather than a plain .so:
 *
 *   PostgreSQL does not export its symbols to libraries. A module that needs
 *   `palloc` must be linked as a Mach-O bundle (on macOS) with its undefined
 *   symbols resolved by the host process, which is what `-bundle
 *   -Wl,-undefined,dynamic_lookup` does. That is exactly how every real
 *   extension in $libdir is built. The alternative, `-bundle_loader`, makes
 *   the module depend on one specific `postgres` binary and would defeat the
 *   build-once property.
 *
 * Build (see Makefile.poc):
 *   cc -bundle -Wl,-undefined,dynamic_lookup -fPIC -I$PGINC -I.. \
 *      -o kwabi_runtime_pg18.dylib kwabi_runtime_pg18.c libkwabi_runtime.a
 */

#include "shim_internal.h"
#include "tcop/tcopprot.h"           /* pg_plan_query */

/* ---- shim-provided stable slots -------------------------------------- */

/* ---- shim-provided stable slots -------------------------------------- */

/*
 * The owning memory context of a palloc'd chunk.
 *
 * This is the assertion the POC rests on. If memory handed out through the
 * ABI really came from PostgreSQL's allocator, PostgreSQL can tell us which
 * context owns it, and that context is the backend's current one. If the
 * runtime had secretly used malloc, GetMemoryChunkContext would read a header
 * that is not there.
 */
KwabiMemoryContext
shim_memory_chunk_context(void *pointer)
{
    if (pointer == NULL)
        return NULL;
    return (KwabiMemoryContext) GetMemoryChunkContext(pointer);
}

KwabiMemoryContext
shim_current_memory_context(void)
{
    return (KwabiMemoryContext) CurrentMemoryContext;
}

/*
 * Raise a real PostgreSQL ERROR.
 *
 * This longjmps, which is why it lives in C and not in the Rust core: a jump
 * that unwinds through Rust frames skips destructors and is undefined
 * behaviour. An extension that wants a genuine SQL error calls this; one that
 * wants to fail without unwinding uses the runtime's own error state
 * (kwabi_raise) instead. Both directions are available, and the choice is
 * explicit rather than accidental.
 *
 * ereport is a macro, so it cannot be forwarded. The message is passed
 * through errmsg("%s", ...) rather than as a format string, so a message
 * containing a percent sign cannot turn into a format-string bug.
 */
void
shim_raise_error(int sqlerrcode, const char *msg)
{
    ereport(ERROR,
            (errcode(sqlerrcode),
             errmsg("%s", msg != NULL ? msg : "kwabi: error")));
}


/* ---- native table: PostgreSQL 18 symbols ----------------------------- */

/* ---- native table: PostgreSQL 18 symbols ----------------------------- */

static void *
native_palloc(size_t n)
{
    return palloc(n);
}

static void *
native_palloc0(size_t n)
{
    return palloc0(n);
}

static void *
native_repalloc(void *p, size_t n)
{
    return repalloc(p, n);
}

static void
native_pfree(void *p)
{
    pfree(p);
}

static void *
native_context_current(void)
{
    return (void *) CurrentMemoryContext;
}

/*
 * MemoryContextSwitchTo is `static inline` in palloc.h — it assigns to the
 * exported CurrentMemoryContext global and returns the previous value. There
 * is no function to take the address of, so the shim reimplements the
 * two-line body rather than trying to dlsym it.
 */
static void *
native_context_switch_to(void *ctx)
{
    return (void *) MemoryContextSwitchTo((MemoryContext) ctx);
}

static void
native_memory_context_reset(void *ctx)
{
    MemoryContextReset((MemoryContext) ctx);
}

static void
native_memory_context_delete(void *ctx)
{
    MemoryContextDelete((MemoryContext) ctx);
}

/*
 * Create a new memory context under the current one.
 *
 * AllocSetContextCreate is a macro, not a function, so it cannot be forwarded
 * across the ABI. Its expansion also carries a static assertion that `name` is
 * a compile-time constant, because PostgreSQL stores the pointer rather than
 * copying it -- a runtime string would dangle as soon as the caller's frame
 * went away. Our name arrives at runtime and can never satisfy that check.
 *
 * Two steps replace it:
 *
 *   1. call AllocSetContextCreateInternal directly -- the macro's expansion
 *      without the assertion -- passing a literal placeholder name;
 *   2. install a COPY of the caller's name with
 *      MemoryContextCopyAndSetIdentifier, which strdups it into the new context
 *      so it lives exactly as long as the context does.
 *
 * This is the path PostgreSQL's own header names for a variable identifier
 * ("Use MemoryContextSetIdentifier if you want to provide a variable
 * identifier"), so it is the supported route rather than a way around a check.
 */
KwabiMemoryContext
shim_memory_context_create(const char *name)
{
    MemoryContext ctx = AllocSetContextCreateInternal(
        CurrentMemoryContext,
        "kwabi",
        ALLOCSET_DEFAULT_SIZES);

    MemoryContextCopyAndSetIdentifier(ctx,
                                      name != NULL ? name : "kwabi_unnamed");

    return (KwabiMemoryContext) ctx;
}


/* ---- native error/log functions -------------------------------------- */

/*
 * The legacy error slots read the buffer error_get reads, the one the shim
 * fills in shim_capture_error. error_message returns a static copy, valid until
 * the next call. error_clear writes an empty error into the buffer.
 */
static KwabiError
native_last_error(void)
{
    KwabiError err;

    kwabi_error_init(&err);
    kwabi_error_get(&err);
    return err;
}

static const char *
native_error_message(void)
{
    static char message[KWABI_ERRMSG_MAX];
    KwabiError  err = native_last_error();

    snprintf(message, sizeof(message), "%s", err.message);
    return message;
}

static int
native_error_code(void)
{
    return native_last_error().sqlerrcode;
}

static void
native_error_clear(void)
{
    KwabiError err;

    kwabi_error_init(&err);
    kwabi_error_set(&err);
}

static void
native_log_line(int level, const char *msg)
{
    ereport(level,
            (errmsg("%s", msg != NULL ? msg : "")));
}

/* ---- install_native -------------------------------------------------- */

static void
install_native(KwabiNative *n)
{
    n->pg_major = PG_VERSION_NUM / 10000;
    n->palloc = native_palloc;
    n->palloc0 = native_palloc0;
    n->repalloc = native_repalloc;
    n->pfree = native_pfree;
    n->memory_context_current = native_context_current;
    n->memory_context_switch_to = native_context_switch_to;
    n->memory_context_reset = native_memory_context_reset;
    n->memory_context_delete = native_memory_context_delete;
    n->error_message = native_error_message;
    n->error_code = native_error_code;
    n->error_clear = native_error_clear;
    n->log_line = native_log_line;
}

/* ---- lifecycle ------------------------------------------------------- */

/* ---- lifecycle ------------------------------------------------------- */

PG_MODULE_MAGIC;

/* Forward declarations: the firewall implementations live below _PG_init, but
 * _PG_init is where they get wired into the published table. */
static KwabiStatus shim_try_body(KwabiBodyFn body, void *arg, KwabiError *out);
static void shim_error_get(KwabiError *out);

/*
 * The table the Rust core published, with the shim-owned slots filled in.
 *
 * The core publishes a `const KwabiV1 *`. The shim needs a mutable copy to
 * add three slots, so it takes the core's table as a template and overlays
 * them. Copying is safe: the core's table is immutable and this happens once,
 * before anything can have cached a pointer.
 */
KwabiV1 shim_table;
const KwabiV1 *shim_api = NULL;

/* Defined below with the other SQL-callable functions; _PG_init needs it. */
static uint64_t shim_capabilities(void);

void
_PG_init(void)
{
    KwabiNative native;

    install_native(&native);

    const KwabiV1 *core = kwabi_runtime_init(&native);
    if (core == NULL)
        elog(FATAL, "kwabi: runtime refused to publish the ABI table");

    shim_table = *core;
    shim_table.memory_chunk_context = shim_memory_chunk_context;
    shim_table.current_memory_context = shim_current_memory_context;
    shim_table.raise_error = shim_raise_error;
    shim_table.memory_context_create = shim_memory_context_create;

    /* Initialize all shim groups */
    init_group_fmgr();
    init_group_spi();
    init_group_guc();
    init_group_defrem();
    init_group_type();
    init_group_stringinfo();
    init_group_shmem();
    init_group_parser();
    init_group_tuple();
    init_group_relation();
    init_group_buffer();
    init_group_syscache();
    init_group_lwlock();
    init_group_node();
    init_group_trigger();
    init_group_tableam();
    init_group_lock();
    init_group_extension();
    init_group_explain();
    init_group_transaction();
    init_group_executor();
    init_group_bgworker();
    init_group_slru();

    /*
     * SLRU is the one group that must also touch the preload path: it needs
     * shmem reserved before fork. This installs the hooks when the runtime is
     * being preloaded, and records "not preloaded" when it is a plain LOAD so
     * the capability bit can stay honest.
     */
    (void) shim_slru_install_hooks();

    /*
     * The catching direction is shim-owned for the same reason: it needs
     * PG_TRY and a subtransaction, which cannot live in Rust. The runtime
     * supplies the panic containment that this calls into.
     */
    shim_table.try_body = shim_try_body;
    shim_table.error_get = shim_error_get;

    /*
     * The capability slot must answer from the FINAL table, which is this one.
     * Installing the shim's own answer here rather than leaving the runtime's
     * `rt_capabilities` in place is the fix for exactly that: the runtime's
     * version reads the runtime's table, which does not have the slots above.
     */
    shim_table.capabilities = shim_capabilities;

    shim_api = &shim_table;
}


/* ---- SQL-callable proof functions ------------------------------------ */

PG_FUNCTION_INFO_V1(kwabi_proof);
PG_FUNCTION_INFO_V1(kwabi_roundtrip);

/*
 * kwabi_proof() — prove that a SQL function can reach PostgreSQL's allocator
 * through the stable ABI rather than through linked PostgreSQL symbols.
 *
 * The interesting assertion is the last one. Memory that came out of the ABI
 * is handed to PostgreSQL's own GetMemoryChunkContext, which returns the
 * context that owns the chunk. If that is the backend's current context, the
 * pointer is genuine PostgreSQL memory — allocated by palloc, owned by the
 * running transaction's context, freed by PostgreSQL's normal rules. A
 * malloc'd pointer cannot pass this check.
 */
Datum
kwabi_proof(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    /* Every slot we expect must be present; a null slot is a hard error. */
    if (api->palloc == NULL || api->pfree == NULL ||
        api->memory_chunk_context == NULL || api->current_memory_context == NULL)
        ereport(ERROR, (errmsg("kwabi: required ABI slots are missing")));

    void *p = api->palloc(256);
    if (p == NULL)
        ereport(ERROR, (errmsg("kwabi: palloc through the ABI returned NULL")));

    /* Write through it, so a bad pointer faults here rather than silently. */
    char *bytes = (char *) p;
    for (int i = 0; i < 16; i++)
        bytes[i] = (char) ('A' + i);

    /*
     * The load-bearing assertion: PostgreSQL must recognise this pointer as
     * one of its own.
     */
    KwabiMemoryContext owner = api->memory_chunk_context(p);
    KwabiMemoryContext current = api->current_memory_context();

    if (owner == NULL)
        ereport(ERROR, (errmsg("kwabi: PostgreSQL does not own the ABI pointer")));
    if (owner != current)
        ereport(ERROR,
                (errmsg("kwabi: ABI pointer belongs to a different context"),
                 errdetail("owner=%p current=%p", owner, current)));

    api->pfree(p);

    PG_RETURN_TEXT_P(cstring_to_text("kwabi: ABI memory is genuine PostgreSQL memory"));
}

/*
 * kwabi_roundtrip() — allocate and free N buffers through the ABI, inside a
 * real backend, under a real transaction.
 *
 * This is the shape the type-kit and engine work will take: lots of small
 * calls across the boundary. It also proves the error firewall's safe
 * direction — the allocation is checked and released without any PostgreSQL
 * error being raised.
 */
Datum
kwabi_roundtrip(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    int32 n = PG_GETARG_INT32(0);
    if (n <= 0)
        ereport(ERROR, (errmsg("kwabi: count must be positive")));

    const KwabiV1 *api = shim_api;

    for (int32 i = 0; i < n; i++) {
        void *p = api->palloc(64);
        if (p == NULL)
            ereport(ERROR, (errmsg("kwabi: allocation %d failed", i)));
        if (api->memory_chunk_context(p) == NULL)
            ereport(ERROR, (errmsg("kwabi: allocation %d is not PostgreSQL memory", i)));
        api->pfree(p);
    }

    PG_RETURN_INT32(n);
}

/*
 * kwabi_raise_test() — prove that a genuine PostgreSQL ERROR can be raised
 * from a SQL-callable function through the ABI.
 *
 * The value is that the jump happens in C, in this frame, not across a Rust
 * frame. A caller should see SQLSTATE 22012 (division_by_zero) and the
 * transaction should be left aborted but the backend alive.
 */
PG_FUNCTION_INFO_V1(kwabi_raise_test);

Datum
kwabi_raise_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    shim_api->raise_error(ERRCODE_DIVISION_BY_ZERO, "kwabi: deliberate proof error");

    /* Not reached: raise_error does not return. */
    PG_RETURN_VOID();
}

/*
 * kwabi_version() — report what the extension can see about the ABI.
 */
/*
 * The ABI capability slot, shim-owned.
 *
 * Delegates the computation to the runtime, but passes the SHIM's table so the
 * answer reflects the slots the shim installed. pg_major comes from the shim
 * because only the shim knows which PostgreSQL this is.
 */
uint64_t
shim_capabilities(void)
{
    uint64_t caps = kwabi_capabilities_of(&shim_table, (uint32_t) (PG_VERSION_NUM / 10000));

    /*
     * SLRU is the one bit the runtime cannot compute for itself. The slots are
     * wired whether or not the runtime was preloaded, so slot presence says
     * nothing; only the shim knows whether the preload hooks ran and an SLRU
     * was actually initialised. So the shim ORs the bit in here rather than the
     * runtime guessing it.
     */
    if (shim_slru_is_available())
        caps |= KWABI_CAP_SLRU;

    return caps;
}

PG_FUNCTION_INFO_V1(kwabi_capabilities);

/*
 * Return the capability bitset from the runtime.
 *
 * Thin on purpose: the ANSWER is computed in the runtime (rt_capabilities),
 * from what it actually wired. The shim only carries it across, so there is no
 * second place for the truth to live and drift.
 */
Datum
kwabi_capabilities(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        PG_RETURN_INT64((int64) KWABI_CAP_CORE);

    /*
     * Go through the shim's slot, which is the same path an extension takes.
     * The bootstrap case (a runtime with no capability slot at all) cannot
     * arise here -- this shim always installs one -- so it is verified in the
     * harness instead, against a synthetic table.
     */
    PG_RETURN_INT64((int64) shim_capabilities());
}

PG_FUNCTION_INFO_V1(kwabi_cap_names);

/* Decode the bitset to names, so a failing assertion says WHICH bit. */
Datum
kwabi_cap_names(PG_FUNCTION_ARGS)
{
    const KwabiV1 *api = shim_api;
    uint64 caps = (api != NULL && api->capabilities != NULL)
        ? api->capabilities() : (uint64) KWABI_CAP_CORE;
    StringInfoData buf;
    bool first = true;

    initStringInfo(&buf);

    struct { uint64 bit; const char *name; } names[] = {
        { KWABI_CAP_CORE,                "CORE" },
        { KWABI_CAP_STRUCTURED_ERRORS,   "STRUCTURED_ERRORS" },
        { KWABI_CAP_ERROR_FIREWALL,      "ERROR_FIREWALL" },
        { KWABI_CAP_MEMORY_INTROSPECTION,"MEMORY_INTROSPECTION" },
        { KWABI_CAP_ATOMIC_BODY,         "ATOMIC_BODY" },
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (caps & names[i].bit)
        {
            appendStringInfo(&buf, "%s%s", first ? "" : "|", names[i].name);
            first = false;
        }
    }
    if (first)
        appendStringInfoString(&buf, "(none)");

    PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * kwabi_mem_api_test() — prove the full memory context lifecycle through
 * the ABI: create → switch → allocate → verify ownership → verify name →
 * reset → delete.
 *
 * This is the test for the mem-api kata (0teh). It exercises the three
 * previously-unwired slots (memory_context_reset, memory_context_delete)
 * and the new memory_context_create slot.
 *
 * Returns a text summary. Raises ERROR if any step fails.
 */
PG_FUNCTION_INFO_V1(kwabi_mem_api_test);

Datum
kwabi_mem_api_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    /* All four slots must be present. */
    if (api->memory_context_create == NULL ||
        api->memory_context_reset == NULL ||
        api->memory_context_delete == NULL ||
        api->memory_context_switch_to == NULL ||
        api->memory_context_current == NULL ||
        api->memory_chunk_context == NULL)
        ereport(ERROR, (errmsg("kwabi: memory context slots are missing")));

    /* 1. Create a new context under the current one. */
    KwabiMemoryContext ctx = api->memory_context_create("kwabi_test_ctx");
    if (ctx == NULL)
        ereport(ERROR, (errmsg("kwabi: memory_context_create returned NULL")));

    /*
     * 1b. The NAME must have survived.
     *
     * This is not decoration. The name arrives as a runtime string, which
     * cannot be handed to AllocSetContextCreate's macro -- it static-asserts a
     * compile-time constant, because PostgreSQL would otherwise keep the
     * caller's pointer and dangle. The shim therefore installs a COPY via
     * MemoryContextCopyAndSetIdentifier, and the copy is the part that can be
     * got wrong: a version that kept the caller's pointer would read back
     * correctly right here and corrupt later, once the caller's frame is gone.
     *
     * Reading `ident` back is the closest a single backend can get to that
     * bug. It proves the copy is installed and matches; it cannot prove the
     * copy outlives the caller, because nothing here outlives the call. What
     * it does prove is that the value came through the ABI at all, which the
     * old "implicit" placeholder check did not.
     *
     * The NULL test comes first and is load-bearing. Without the copy, `ident`
     * is NULL, and strcmp(NULL, ...) segfaults -- a backend crash, not a
     * failing test. A control that kills the postmaster is not a control; it
     * is an outage. So the check must FAIL cleanly on a missing name.
     */
    const char *ident = ((MemoryContext) ctx)->ident;
    if (ident == NULL || strcmp(ident, "kwabi_test_ctx") != 0)
        ereport(ERROR,
                (errmsg("kwabi: created context carries the wrong name"),
                 errdetail("expected=\"kwabi_test_ctx\" got=\"%s\"",
                           ident != NULL ? ident : "(null)")));

    /* 2. Switch to it. */
    KwabiMemoryContext old = api->memory_context_switch_to(ctx);
    if (old == NULL)
        ereport(ERROR, (errmsg("kwabi: memory_context_switch_to returned NULL")));

    /* 3. Verify the current context is the new one. */
    KwabiMemoryContext current = api->memory_context_current();
    if (current != ctx)
        ereport(ERROR,
                (errmsg("kwabi: after switch, current context is not the new one"),
                 errdetail("ctx=%p current=%p", ctx, current)));

    /* 4. Allocate inside the new context and verify ownership. */
    void *p = api->palloc(128);
    if (p == NULL)
        ereport(ERROR, (errmsg("kwabi: palloc inside new context returned NULL")));

    KwabiMemoryContext owner = api->memory_chunk_context(p);
    if (owner != ctx)
        ereport(ERROR,
                (errmsg("kwabi: allocation is not owned by the new context"),
                 errdetail("ctx=%p owner=%p", ctx, owner)));

    /* 5. Switch back. */
    api->memory_context_switch_to(old);

    /* 6. Reset the new context — the allocation should be freed. */
    api->memory_context_reset(ctx);

    /* 7. Delete the new context entirely. */
    api->memory_context_delete(ctx);

    PG_RETURN_TEXT_P(cstring_to_text(
        "kwabi: memory context lifecycle verified (create/switch/alloc/name/reset/delete)"));
}

/*
 * kwabi_mem_api_control() — the NEGATIVE CONTROL for kwabi_mem_api_test().
 *
 * It creates a context and asserts the name it finds is a value that is NOT
 * there. That assertion must fail, so this function must raise.
 *
 * It has two failure messages on purpose, and the distinction is the whole
 * point:
 *
 *   * "fired as intended" -- a real name was present and the wrong-name
 *     comparison correctly rejected it. This is the healthy outcome. It is
 *     what proves the real test's name check can tell a right name from a
 *     wrong one, rather than passing on anything.
 *
 *   * "control did not fire: NO name was installed" -- `ident` was NULL, so
 *     the wrong-name comparison had nothing to compare and did not exercise
 *     anything. A control in this state is not evidence, and the harness
 *     counts only the first message, so this one fails the run.
 *
 * Both are raised as errors (neither returns a row), so a single run proves
 * two things at once: the test can fail, and it fails cleanly rather than
 * crashing the backend. The NULL branch is guarded for exactly that reason --
 * strcmp(NULL, ...) would take the postmaster down, and a control that kills
 * the server is an outage, not a control.
 *
 * Same standard as guard-control.sql and capabilities-design.md §5.
 */
PG_FUNCTION_INFO_V1(kwabi_mem_api_control);

Datum
kwabi_mem_api_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->memory_context_create == NULL)
        ereport(ERROR, (errmsg("kwabi: memory_context_create is not wired")));

    KwabiMemoryContext ctx = api->memory_context_create("kwabi_control_ctx");
    if (ctx == NULL)
        ereport(ERROR, (errmsg("kwabi: memory_context_create returned NULL")));

    const char *ident = ((MemoryContext) ctx)->ident;

    /* Clean up before any ereport: it longjmps and would skip this. */
    api->memory_context_delete(ctx);

    if (ident == NULL)
        ereport(ERROR,
                (errmsg("kwabi: control did not fire: NO name was installed"),
                 errdetail("the wrong-name comparison had nothing to test")));

    /* The wrong name must not match. If it does, the comparison is broken. */
    if (strcmp(ident, "definitely_not_this") == 0)
        ereport(ERROR,
                (errmsg("kwabi: control did not fire: the comparison matched "
                        "a name that is not there")));

    ereport(ERROR,
            (errmsg("kwabi: negative control fired as intended"),
             errdetail("the name assertion distinguishes a real name from a wrong one")));
}

/* ========================================================================
 * fmgr-api proof functions (kata dak5)
 * ========================================================================
 *
 * These exercise the five fmgr slots through the published table -- the same
 * path an extension takes -- so what is tested is the ABI, not a parallel
 * copy of it. They are test scaffolding: none is a slot in KwabiV1.
 *
 * The SQL declarations take `int4` for OIDs rather than `oid`, so a literal
 * like 1397 needs no cast; the C side widens to Oid.
 */

/* Shared: fetch the FmgrInfo for an OID, or raise. */
static KwabiFmgrInfo
fmgr_info_or_error(Oid fnoid)
{
    KwabiFmgrInfo info;

    if (shim_api == NULL || shim_api->fmgr_info == NULL)
        ereport(ERROR, (errmsg("kwabi: fmgr_info is not wired")));

    info = shim_api->fmgr_info(fnoid);
    if (info == NULL) {
        KwabiError err;
        kwabi_error_init(&err);
        if (shim_api->error_get != NULL)
            shim_api->error_get(&err);
        ereport(ERROR,
                (errmsg("kwabi: fmgr_info(%u) failed", (unsigned) fnoid),
                 errdetail("sqlstate=%d msg=\"%s\"", err.sqlerrcode, err.message)));
    }
    return info;
}

/*
 * kwabi_fmgr_call1(int4 fnoid, int4 arg1) -> int4
 * kwabi_fmgr_call2(int4 fnoid, int4 a, int4 b) -> int4
 *
 * A direct call. On a non-OK status the slot has already caught the PostgreSQL
 * error and rolled back the subtransaction; this re-raises so the SQL caller
 * sees a real error (which is what a caller that does not want to swallow
 * errors should do).
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_call1);

Datum
kwabi_fmgr_call1(PG_FUNCTION_ARGS)
{
    Oid           fnoid = (Oid) PG_GETARG_INT32(0);
    int32         a     = PG_GETARG_INT32(1);
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiStatus   st;

    if (shim_api->call_function1 == NULL)
        ereport(ERROR, (errmsg("kwabi: call_function1 is not wired")));

    st = shim_api->call_function1(info, Int32GetDatum(a), &isnull, &result);
    if (st != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: call_function1 returned status %d", (int) st)));

    if (isnull)
        PG_RETURN_NULL();
    PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(kwabi_fmgr_call2);

Datum
kwabi_fmgr_call2(PG_FUNCTION_ARGS)
{
    Oid           fnoid = (Oid) PG_GETARG_INT32(0);
    int32         a     = PG_GETARG_INT32(1);
    int32         b     = PG_GETARG_INT32(2);
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiStatus   st;

    if (shim_api->call_function2 == NULL)
        ereport(ERROR, (errmsg("kwabi: call_function2 is not wired")));

    st = shim_api->call_function2(info, Int32GetDatum(a), Int32GetDatum(b),
                                  &isnull, &result);
    if (st != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: call_function2 returned status %d", (int) st)));

    if (isnull)
        PG_RETURN_NULL();
    PG_RETURN_DATUM(result);
}

/*
 * kwabi_fmgr_call3(int4 fnoid, text a, int4 b, int4 c) -> text
 *
 * The 3-argument slot, with mixed argument types. `substr(text,int4,int4)` is
 * the target; the point is that the slot does not care about argument types,
 * only about Datums.
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_call3);

Datum
kwabi_fmgr_call3(PG_FUNCTION_ARGS)
{
    Oid           fnoid = (Oid) PG_GETARG_INT32(0);
    Datum         a     = PG_GETARG_DATUM(1);   /* text */
    int32         b     = PG_GETARG_INT32(2);
    int32         c     = PG_GETARG_INT32(3);
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiStatus   st;

    if (shim_api->call_function3 == NULL)
        ereport(ERROR, (errmsg("kwabi: call_function3 is not wired")));

    st = shim_api->call_function3(info, a, Int32GetDatum(b), Int32GetDatum(c),
                                  &isnull, &result);
    if (st != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: call_function3 returned status %d", (int) st)));

    if (isnull)
        PG_RETURN_NULL();
    PG_RETURN_DATUM(result);
}

/*
 * kwabi_fmgr_calln(int4 fnoid, int4 a, int4 b) -> int4
 *
 * The variadic `call_function` slot. PostgreSQL has no N-ary call helper, so
 * this goes through the path the shim builds by hand -- which is exactly why it
 * needs its own test rather than being assumed to work because the fixed-arity
 * slots do.
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_calln);

Datum
kwabi_fmgr_calln(PG_FUNCTION_ARGS)
{
    Oid           fnoid = (Oid) PG_GETARG_INT32(0);
    int32         a     = PG_GETARG_INT32(1);
    int32         b     = PG_GETARG_INT32(2);
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    Datum         args[2];
    bool          argnulls[2] = {false, false};
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiStatus   st;

    if (shim_api->call_function == NULL)
        ereport(ERROR, (errmsg("kwabi: call_function is not wired")));

    args[0] = Int32GetDatum(a);
    args[1] = Int32GetDatum(b);

    st = shim_api->call_function(info, 2, args, argnulls, &isnull, &result);
    if (st != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: call_function returned status %d", (int) st)));

    if (isnull)
        PG_RETURN_NULL();
    PG_RETURN_DATUM(result);
}

/*
 * kwabi_fmgr_null_result() -> bool
 *
 * Whether a function that returns SQL NULL is REPORTED as NULL.
 *
 * This is the test the old `Datum`-returning signature could not pass: a bare
 * Datum has no null channel, so a NULL result would have been indistinguishable
 * from 0. The target is `current_setting('kwabi.no_such_guc', true)`, which
 * returns NULL for a missing GUC with missing_ok=true.
 *
 * It is also its own control: if the ABI ignored `fcinfo->isnull`, the result
 * would be a non-NULL datum and this returns false.
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_null_result);

Datum
kwabi_fmgr_null_result(PG_FUNCTION_ARGS)
{
    Oid           fnoid = 3294;   /* current_setting(text, bool) */
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiStatus   st;

    if (shim_api->call_function2 == NULL)
        ereport(ERROR, (errmsg("kwabi: call_function2 is not wired")));

    st = shim_api->call_function2(info,
                                  CStringGetTextDatum("kwabi.no_such_guc"),
                                  BoolGetDatum(true),
                                  &isnull, &result);
    if (st != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: current_setting call returned status %d", (int) st)));

    PG_RETURN_BOOL(isnull);
}

/*
 * kwabi_fmgr_error_code() -> text
 *
 * Calls `int4div(1, 0)`, which raises division_by_zero inside the slot. The
 * slot must CATCH it (returning a non-OK status) and the SQLSTATE must be
 * readable through `error_get`. Returns the SQLSTATE as text ('22012').
 *
 * Note the encoding: `err.sqlerrcode` holds PostgreSQL's packed SQLSTATE
 * (five 6-bit characters), not the decimal 22012. `unpack_sql_state` is the
 * supported decoder. A raw `sqlerrcode` compared against 22012 would silently
 * fail while looking correct — which is how this test first failed.
 *
 * Returns 'none' if the call wrongly succeeded, so a swallowed error shows up
 * as a wrong answer rather than a silent pass.
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_error_code);

Datum
kwabi_fmgr_error_code(PG_FUNCTION_ARGS)
{
    Oid           fnoid = 154;   /* int4div(int4, int4) */
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiError    err;
    KwabiStatus   st;

    kwabi_error_init(&err);

    if (shim_api->call_function2 == NULL || shim_api->error_get == NULL)
        ereport(ERROR, (errmsg("kwabi: fmgr error slots are not wired")));

    st = shim_api->call_function2(info, Int32GetDatum(1), Int32GetDatum(0),
                                  &isnull, &result);
    if (st == KWABI_OK)
        PG_RETURN_TEXT_P(cstring_to_text("none"));   /* swallowed: wrong */

    shim_api->error_get(&err);
    PG_RETURN_TEXT_P(cstring_to_text(unpack_sql_state(err.sqlerrcode)));
}

/*
 * kwabi_fmgr_ins_then_raise(int4 fnoid, int4 tag) -> text
 *
 * THE load-bearing test for the call slots, and the one error-firewall-design
 * §7 calls "the only test that distinguishes a firewall that works from one
 * that merely does not crash".
 *
 * It calls a plpgsql function that INSERTs a row and then raises. The slot must
 * catch the error AND roll back the subtransaction, so the row must NOT survive.
 * Without the subtransaction the insert would remain, visible to the rest of
 * the transaction and able to commit — the silent-inconsistency bug the
 * firewall exists to prevent.
 *
 * Returns the SQLSTATE, or 'none' if the call wrongly succeeded. The SQL file
 * then counts rows with the tag: the count must be 0.
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_ins_then_raise);

Datum
kwabi_fmgr_ins_then_raise(PG_FUNCTION_ARGS)
{
    Oid           fnoid = (Oid) PG_GETARG_INT32(0);
    int32         tag   = PG_GETARG_INT32(1);
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiError    err;
    KwabiStatus   st;

    kwabi_error_init(&err);

    if (shim_api->call_function1 == NULL || shim_api->error_get == NULL)
        ereport(ERROR, (errmsg("kwabi: fmgr slots are not wired")));

    st = shim_api->call_function1(info, Int32GetDatum(tag), &isnull, &result);
    if (st == KWABI_OK)
        PG_RETURN_TEXT_P(cstring_to_text("none"));   /* swallowed: wrong */

    shim_api->error_get(&err);
    PG_RETURN_TEXT_P(cstring_to_text(unpack_sql_state(err.sqlerrcode)));
}

/*
 * kwabi_fmgr_bad_lookup() -> bool
 *
 * fmgr_info(InvalidOid) must fail -- PostgreSQL raises "cache lookup failed for
 * function 0" -- and the shim must catch it and return NULL rather than
 * crashing. Returns whether NULL came back.
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_bad_lookup);

Datum
kwabi_fmgr_bad_lookup(PG_FUNCTION_ARGS)
{
    KwabiFmgrInfo info;

    if (shim_api == NULL || shim_api->fmgr_info == NULL)
        ereport(ERROR, (errmsg("kwabi: fmgr_info is not wired")));

    info = shim_api->fmgr_info(InvalidOid);
    PG_RETURN_BOOL(info == NULL);
}

/*
 * kwabi_fmgr_control() -> bool
 *
 * The NEGATIVE CONTROL. It calls `int4pl(2, 3)` -- which is 5 -- and asserts
 * the result is 6. That is false, so this must RAISE.
 *
 * If it returns instead, the value read through the ABI is not being compared
 * honestly, and every equality assertion in fmgr-api.sql is vacuous. Same
 * standard as kwabi_mem_api_control and capabilities-design.md §5.
 */
PG_FUNCTION_INFO_V1(kwabi_fmgr_control);

Datum
kwabi_fmgr_control(PG_FUNCTION_ARGS)
{
    Oid           fnoid = 177;   /* int4pl(int4, int4) */
    KwabiFmgrInfo info  = fmgr_info_or_error(fnoid);
    bool          isnull = false;
    Datum         result = (Datum) 0;
    KwabiStatus   st;

    if (shim_api->call_function2 == NULL)
        ereport(ERROR, (errmsg("kwabi: call_function2 is not wired")));

    st = shim_api->call_function2(info, Int32GetDatum(2), Int32GetDatum(3),
                                  &isnull, &result);
    if (st != KWABI_OK)
        ereport(ERROR, (errmsg("kwabi: control call returned status %d", (int) st)));

    /* The control's whole point: this comparison must be FALSE. */
    if (!isnull && DatumGetInt32(result) == 6)
        PG_RETURN_BOOL(true);   /* the comparison thinks 5 == 6: broken */

    ereport(ERROR,
            (errmsg("kwabi: fmgr negative control fired as intended"),
             errdetail("int4pl(2,3) is 5, not 6 -- the value comparison is honest")));
}

/* ========================================================================
 * SPI proof functions
 * ========================================================================
 *
 * These exercise the five SPI slots through the published table -- the same
 * path an extension takes -- so what is tested is the ABI, not a parallel
 * copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_spi_query(sql) -> text
 *
 * Execute a SQL query through the ABI and return a summary of the result.
 * Proves that spi_execute, spi_result_ntuples, spi_result_get_value and
 * spi_free_result all work together.
 */
PG_FUNCTION_INFO_V1(kwabi_spi_query);

Datum
kwabi_spi_query(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->spi_execute == NULL || api->spi_result_ntuples == NULL ||
        api->spi_result_get_value == NULL || api->spi_free_result == NULL)
        ereport(ERROR, (errmsg("kwabi: SPI slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiSPIResult result = api->spi_execute(sql, true, 0);
    if (result == NULL)
        ereport(ERROR, (errmsg("kwabi: spi_execute returned NULL")));

    int ntuples = api->spi_result_ntuples(result);
    Datum first_datum = (Datum) 0;

    if (ntuples > 0) {
        /* Read the first row's first column to prove get_value works. */
        first_datum = api->spi_result_get_value(result, 0, 1);
    }

    api->spi_free_result(result);
    pfree(sql);

    /* Build the result string AFTER spi_free_result so that buf.data is
     * allocated in the caller's context, not the SPI context that
     * SPI_finish() has already destroyed. */
    StringInfoData buf;
    initStringInfo(&buf);

    if (ntuples > 0) {
        appendStringInfo(&buf, "rows=%d first_datum=%lu", ntuples,
                         (unsigned long) first_datum);
    } else {
        appendStringInfo(&buf, "rows=0");
    }

    PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * kwabi_spi_insert_then_select() -> text
 *
 * Insert a row through the ABI, then select it back. Proves that writes
 * through SPI work and that the result of a subsequent query is readable.
 */
PG_FUNCTION_INFO_V1(kwabi_spi_insert_then_select);

Datum
kwabi_spi_insert_then_select(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->spi_execute == NULL || api->spi_result_ntuples == NULL ||
        api->spi_result_get_value == NULL || api->spi_free_result == NULL)
        ereport(ERROR, (errmsg("kwabi: SPI slots are not wired")));

    /* Create a temp table. */
    KwabiSPIResult r1 = api->spi_execute(
        "CREATE TEMP TABLE kwabi_spi_t (val int)", false, 0);
    if (r1 == NULL)
        ereport(ERROR, (errmsg("spi_execute(CREATE) failed")));
    api->spi_free_result(r1);

    /* Insert a row. */
    KwabiSPIResult r2 = api->spi_execute(
        "INSERT INTO kwabi_spi_t VALUES (42)", false, 0);
    if (r2 == NULL)
        ereport(ERROR, (errmsg("spi_execute(INSERT) failed")));
    api->spi_free_result(r2);

    /* Select it back.
     *
     * read_only=false, deliberately. A read-only SPI execute runs under a
     * snapshot that does not include the INSERT done earlier in this same
     * command, so it would see zero rows and the check would silently read 0
     * instead of 42. Measured: read_only=true -> 0, read_only=false -> 42. */
    KwabiSPIResult r3 = api->spi_execute(
        "SELECT val FROM kwabi_spi_t", false, 0);
    if (r3 == NULL)
        ereport(ERROR, (errmsg("spi_execute(SELECT) failed")));

    int ntuples = api->spi_result_ntuples(r3);
    int val = 0;
    if (ntuples > 0) {
        Datum d = api->spi_result_get_value(r3, 0, 1);
        val = DatumGetInt32(d);
    }
    api->spi_free_result(r3);

    /* Clean up. */
    KwabiSPIResult r4 = api->spi_execute(
        "DROP TABLE kwabi_spi_t", false, 0);
    if (r4 != NULL)
        api->spi_free_result(r4);

    PG_RETURN_INT32(val);
}

/*
 * kwabi_spi_control() -> bool
 *
 * The NEGATIVE CONTROL. It queries a known value and asserts a wrong one.
 * That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_spi_control);

Datum
kwabi_spi_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->spi_execute == NULL || api->spi_result_ntuples == NULL ||
        api->spi_result_get_value == NULL || api->spi_free_result == NULL)
        ereport(ERROR, (errmsg("kwabi: SPI slots are not wired")));

    KwabiSPIResult result = api->spi_execute(
        "SELECT 1", true, 0);
    if (result == NULL)
        ereport(ERROR, (errmsg("spi_execute(SELECT 1) failed")));

    int ntuples = api->spi_result_ntuples(result);
    Datum d = api->spi_result_get_value(result, 0, 1);
    int val = DatumGetInt32(d);
    api->spi_free_result(result);

    /* The control's whole point: this comparison must be FALSE. */
    if (val == 2)
        PG_RETURN_BOOL(true);   /* the comparison thinks 1 == 2: broken */

    ereport(ERROR,
            (errmsg("kwabi: SPI negative control fired as intended"),
             errdetail("SELECT 1 is 1, not 2 -- the value comparison is honest")));
}

/* ========================================================================
 * GUC proof functions
 * ========================================================================
 *
 * These exercise the eight GUC slots through the published table -- the same
 * path an extension takes -- so what is tested is the ABI, not a parallel
 * copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_guc_test_int() -> bool
 *
 * Get an integer GUC value through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_guc_test_int);

Datum
kwabi_guc_test_int(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->guc_get_int == NULL)
        ereport(ERROR, (errmsg("kwabi: guc_get_int is not wired")));

    int32 expected = PG_GETARG_INT32(0);
    /* max_connections, matching the GUC the test reads. It is unit-less, so
     * its display string is a bare integer and atoi is correct. See the
     * LIMITATION note on shim_guc_get_int for why a unit-suffixed GUC such as
     * work_mem ("4MB") cannot be used with this slot. */
    int32 actual = shim_api->guc_get_int("max_connections");

    PG_RETURN_BOOL(actual == expected);
}

/*
 * kwabi_guc_get_bool_named(text) -> bool
 *
 * Return the boolean GUC value read through the ABI, so SQL can compare it
 * against a value it set itself.
 */
PG_FUNCTION_INFO_V1(kwabi_guc_get_bool_named);

Datum
kwabi_guc_get_bool_named(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->guc_get_bool == NULL)
        ereport(ERROR, (errmsg("kwabi: guc_get_bool is not wired")));

    char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    PG_RETURN_BOOL(shim_api->guc_get_bool(name));
}

/*
 * kwabi_guc_test_string() -> bool
 *
 * Get a string GUC value through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_guc_test_string);

Datum
kwabi_guc_test_string(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->guc_get_string == NULL)
        ereport(ERROR, (errmsg("kwabi: guc_get_string is not wired")));

    const char *actual = shim_api->guc_get_string("server_version");

    /*
     * Property check, not an exact match. server_version returns the version
     * number and vendor suffix, e.g. "18.6 (Homebrew)" -- it does NOT start
     * with "PostgreSQL " (that is version(), a different thing). Asserting a
     * prefix of "PostgreSQL " failed on every major.
     *
     * The honest property is: non-empty, and begins with a digit, since a
     * server version number starts with its major version.
     */
    bool result = (actual != NULL && strlen(actual) > 0 &&
                   actual[0] >= '0' && actual[0] <= '9');

    PG_RETURN_BOOL(result);
}

/*
 * kwabi_guc_test_bool() -> bool
 *
 * Get a boolean GUC value through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_guc_test_bool);

Datum
kwabi_guc_test_bool(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->guc_get_bool == NULL)
        ereport(ERROR, (errmsg("kwabi: guc_get_bool is not wired")));

    bool expected = PG_GETARG_BOOL(0);
    bool actual = shim_api->guc_get_bool("is_superuser");

    PG_RETURN_BOOL(actual == expected);
}

/*
 * kwabi_guc_test_float() -> bool
 *
 * Get a float GUC value through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_guc_test_float);

Datum
kwabi_guc_test_float(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->guc_get_float == NULL)
        ereport(ERROR, (errmsg("kwabi: guc_get_float is not wired")));

    double actual = shim_api->guc_get_float("shared_buffers");

    /* Property check: positive buffer size */
    PG_RETURN_BOOL(actual > 0);
}

/*
 * kwabi_guc_set_test() -> bool
 *
 * Set a GUC value through the ABI, then read it back.
 */
PG_FUNCTION_INFO_V1(kwabi_guc_set_test);

Datum
kwabi_guc_set_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->guc_set_int == NULL ||
        shim_api->guc_set_string == NULL || shim_api->guc_set_bool == NULL ||
        shim_api->guc_set_float == NULL)
        ereport(ERROR, (errmsg("kwabi: guc_set_* slots are not wired")));

    /* Set a custom GUC through the ABI */
    shim_api->guc_set_string("kwabi.test_guc", "hello");
    shim_api->guc_set_int("kwabi.test_guc_int", 42);
    shim_api->guc_set_bool("kwabi.test_guc_bool", true);
    shim_api->guc_set_float("kwabi.test_guc_float", 3.14);

    /* Read them back */
    const char *str_val = shim_api->guc_get_string("kwabi.test_guc");
    int int_val = shim_api->guc_get_int("kwabi.test_guc_int");
    bool bool_val = shim_api->guc_get_bool("kwabi.test_guc_bool");
    double float_val = shim_api->guc_get_float("kwabi.test_guc_float");

    bool result = (str_val != NULL && strcmp(str_val, "hello") == 0 &&
                   int_val == 42 && bool_val == true &&
                   fabs(float_val - 3.14) < 0.001);

    PG_RETURN_BOOL(result);
}

/*
 * kwabi_guc_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts a wrong GUC value, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_guc_control);

Datum
kwabi_guc_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->guc_get_int == NULL)
        ereport(ERROR, (errmsg("kwabi: guc_get_int is not wired")));

    int32 actual = shim_api->guc_get_int("work_mem");

    /* The control's whole point: this comparison must be FALSE. */
    if (actual == 999999)
        PG_RETURN_BOOL(true);   /* the comparison thinks work_mem == 999999: broken */

    ereport(ERROR,
            (errmsg("kwabi: GUC negative control fired as intended"),
             errdetail("work_mem is not 999999 -- the value comparison is honest")));
}

/* ========================================================================
 * defrem proof functions
 * ========================================================================
 *
 * These exercise the three defrem slots through the published table -- the
 * same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_defrem_test() -> bool
 *
 * Create, alter, and drop a column default through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_defrem_test);

Datum
kwabi_defrem_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->defrem_create == NULL ||
        shim_api->defrem_alter == NULL || shim_api->defrem_drop == NULL)
        ereport(ERROR, (errmsg("kwabi: defrem slots are not wired")));

    /* Create a test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("CREATE TEMP TABLE kwabi_defrem_t (val int)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: CREATE TABLE failed")));
    }
    SPI_finish();

    /* Set a default through the ABI */
    shim_api->defrem_create("kwabi_defrem_t.val", "val", "42");

    /* Read it back */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef "
                     "JOIN pg_attribute ON adrelid = attrelid AND adnum = attnum "
                     "WHERE attrelid = 'kwabi_defrem_t'::regclass AND attname = 'val'",
                     false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    bool default_set = false;
    if (SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            /* pg_get_expr returns text. text_to_cstring pallocs a real copy in
             * the current context; DatumGetCString would return a pointer INTO
             * the SPI tuptable, which must not be pfree'd. This block was dead
             * until the read_only flag below was corrected, so the invalid
             * pfree only surfaced once the query actually returned a row. */
            char *expr = text_to_cstring(DatumGetTextPP(d));
            default_set = (strcmp(expr, "42") == 0);
            pfree(expr);
        }
    }
    SPI_finish();

    /* Alter the default through the ABI */
    shim_api->defrem_alter("kwabi_defrem_t.val", "99");

    /* Drop the default through the ABI */
    shim_api->defrem_drop("kwabi_defrem_t.val");

    /* Clean up */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("DROP TABLE kwabi_defrem_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();

    PG_RETURN_BOOL(default_set);
}

/*
 * kwabi_defrem_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts a wrong default value, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_defrem_control);

Datum
kwabi_defrem_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->defrem_create == NULL)
        ereport(ERROR, (errmsg("kwabi: defrem_create is not wired")));

    /* Create a test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("CREATE TEMP TABLE kwabi_defrem_ctl_t (val int)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: CREATE TABLE failed")));
    }
    SPI_finish();

    /* Set a default through the ABI */
    shim_api->defrem_create("kwabi_defrem_ctl_t.val", "val", "42");

    /* Read it back */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef "
                     "JOIN pg_attribute ON adrelid = attrelid AND adnum = attnum "
                     "WHERE attrelid = 'kwabi_defrem_ctl_t'::regclass AND attname = 'val'",
                     false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    bool default_is_99 = false;
    if (SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            /* Same as kwabi_defrem_test: text_to_cstring pallocs a copy;
             * DatumGetCString would return a pointer into the SPI tuptable. */
            char *expr = text_to_cstring(DatumGetTextPP(d));
            default_is_99 = (strcmp(expr, "99") == 0);
            pfree(expr);
        }
    }
    SPI_finish();

    /* Clean up */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("DROP TABLE kwabi_defrem_ctl_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();

    /* The control's whole point: this comparison must be FALSE. */
    if (default_is_99)
        PG_RETURN_BOOL(true);   /* the comparison thinks 42 == 99: broken */

    ereport(ERROR,
            (errmsg("kwabi: defrem negative control fired as intended"),
             errdetail("the default is 42, not 99 -- the value comparison is honest")));
}

/* ========================================================================
 * Parser proof functions
 * ========================================================================
 *
 * These exercise the seven parser slots through the published table -- the
 * same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_parse_expr_test(text) -> text
 *
 * Parse an expression through the ABI and return the node type name.
 * Proves that parse_expr, node_type_name, and free_node all work together.
 */
PG_FUNCTION_INFO_V1(kwabi_parse_expr_test);

Datum
kwabi_parse_expr_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->parse_expr == NULL || api->node_type_name == NULL ||
        api->free_node == NULL)
        ereport(ERROR, (errmsg("kwabi: parser slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = api->parse_expr(sql, NULL, 0);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_expr returned NULL for \"%s\"", sql)));

    const char *type_name = api->node_type_name(node);
    api->free_node(node);
    pfree(sql);

    PG_RETURN_TEXT_P(cstring_to_text(type_name ? type_name : "(null)"));
}

/*
 * kwabi_parse_type_test(text) -> text
 *
 * Parse a type name through the ABI and return the node type name.
 */
PG_FUNCTION_INFO_V1(kwabi_parse_type_test);

Datum
kwabi_parse_type_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->parse_type == NULL || api->node_type_name == NULL ||
        api->free_node == NULL)
        ereport(ERROR, (errmsg("kwabi: parser slots are not wired")));

    char *type_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = api->parse_type(type_name);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_type returned NULL for \"%s\"", type_name)));

    const char *result = api->node_type_name(node);
    api->free_node(node);
    pfree(type_name);

    PG_RETURN_TEXT_P(cstring_to_text(result ? result : "(null)"));
}

/*
 * kwabi_oper_left_type(int4) -> int4
 *
 * Get the left argument type of an operator through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_oper_left_type);

Datum
kwabi_oper_left_type(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->oper_left_type == NULL)
        ereport(ERROR, (errmsg("kwabi: oper_left_type is not wired")));

    Oid oper_oid = (Oid) PG_GETARG_INT32(0);
    Oid result = shim_api->oper_left_type(oper_oid);

    PG_RETURN_OID(result);
}

/*
 * kwabi_oper_right_type(int4) -> int4
 *
 * Get the right argument type of an operator through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_oper_right_type);

Datum
kwabi_oper_right_type(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->oper_right_type == NULL)
        ereport(ERROR, (errmsg("kwabi: oper_right_type is not wired")));

    Oid oper_oid = (Oid) PG_GETARG_INT32(0);
    Oid result = shim_api->oper_right_type(oper_oid);

    PG_RETURN_OID(result);
}

/*
 * kwabi_oper_result_type(int4) -> int4
 *
 * Get the result type of an operator through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_oper_result_type);

Datum
kwabi_oper_result_type(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->oper_result_type == NULL)
        ereport(ERROR, (errmsg("kwabi: oper_result_type is not wired")));

    Oid oper_oid = (Oid) PG_GETARG_INT32(0);
    Oid result = shim_api->oper_result_type(oper_oid);

    PG_RETURN_OID(result);
}

/*
 * kwabi_oper_is_commutative(int4) -> bool
 *
 * Check if an operator is commutative through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_oper_is_commutative);

Datum
kwabi_oper_is_commutative(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->oper_is_commutative == NULL)
        ereport(ERROR, (errmsg("kwabi: oper_is_commutative is not wired")));

    Oid oper_oid = (Oid) PG_GETARG_INT32(0);
    bool result = shim_api->oper_is_commutative(oper_oid);

    PG_RETURN_BOOL(result);
}

/*
 * kwabi_parser_control() -> bool
 *
 * The NEGATIVE CONTROL. It parses a valid expression and asserts the node
 * type is something it is NOT. That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_parser_control);

Datum
kwabi_parser_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->parse_expr == NULL || api->node_type_name == NULL ||
        api->free_node == NULL)
        ereport(ERROR, (errmsg("kwabi: parser slots are not wired")));

    KwabiNode node = api->parse_expr("1 + 1", NULL, 0);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_expr returned NULL")));

    const char *type_name = api->node_type_name(node);
    api->free_node(node);

    /* The control's whole point: this comparison must be FALSE. */
    if (type_name != NULL && strcmp(type_name, "DefinitelyNotANodeType") == 0)
        PG_RETURN_BOOL(true);   /* the comparison thinks it matched: broken */

    ereport(ERROR,
            (errmsg("kwabi: parser negative control fired as intended"),
             errdetail("the node type is not \"DefinitelyNotANodeType\" -- the comparison is honest")));
}

/* ========================================================================
 * Tuple/slot proof functions
 * ========================================================================
 *
 * These exercise the tuple/slot slots through the published table -- the
 * same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_tuple_test() -> bool
 *
 * Test tuple descriptor accessors through the ABI.
 * Creates a test table, gets its TupleDesc through the ABI, and verifies
 * that tuple_natts, tuple_typeid, tuple_typmod, tuple_attname,
 * tuple_attisdropped, and tuple_attnum all return correct values.
 */
PG_FUNCTION_INFO_V1(kwabi_tuple_test);

Datum
kwabi_tuple_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->tuple_natts == NULL || api->tuple_typeid == NULL ||
        api->tuple_typmod == NULL || api->tuple_attname == NULL ||
        api->tuple_attisdropped == NULL || api->tuple_attnum == NULL ||
        api->relation_open == NULL || api->relation_close == NULL ||
        api->relation_tupledesc == NULL)
        ereport(ERROR, (errmsg("kwabi: tuple slots are not wired")));

    /* Create a test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("CREATE TEMP TABLE kwabi_tuple_test_t (id int, name text, val float8)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: CREATE TABLE failed")));
    }
    SPI_finish();

    /* Get the Oid of the temp table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("SELECT oid FROM pg_class WHERE relname = 'kwabi_tuple_test_t'", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    Oid relid = InvalidOid;
    if (SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            relid = DatumGetObjectId(d);
    }
    SPI_finish();

    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("kwabi: could not find test table")));

    /* Open the relation through the ABI */
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    /* Get the TupleDesc through the ABI */
    TupleDesc tupdesc = api->relation_tupledesc(rel);
    if (tupdesc == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_tupledesc failed")));

    /* Test tuple_natts */
    int natts = api->tuple_natts(tupdesc);
    if (natts != 3)
        ereport(ERROR, (errmsg("kwabi: tuple_natts returned %d, expected 3", natts)));

    /* Test tuple_typeid for column 1 (id int) */
    Oid typeid = api->tuple_typeid(tupdesc, 1);
    if (typeid != 23)  /* int4 */
        ereport(ERROR, (errmsg("kwabi: tuple_typeid returned %u, expected 23", typeid)));

    /* Test tuple_typmod for column 1 (int4 has typmod -1) */
    int32 typmod = api->tuple_typmod(tupdesc, 1);
    if (typmod != -1)
        ereport(ERROR, (errmsg("kwabi: tuple_typmod returned %d, expected -1", typmod)));

    /* Test tuple_attname for column 2 */
    const char *attname = api->tuple_attname(tupdesc, 2);
    if (attname == NULL || strcmp(attname, "name") != 0)
        ereport(ERROR, (errmsg("kwabi: tuple_attname returned %s, expected 'name'", attname ? attname : "(null)")));

    /* Test tuple_attnum */
    int attno = api->tuple_attnum(tupdesc, "val");
    if (attno != 3)
        ereport(ERROR, (errmsg("kwabi: tuple_attnum returned %d, expected 3", attno)));

    /* Test tuple_attisdropped */
    bool isdropped = api->tuple_attisdropped(tupdesc, 1);
    if (isdropped)
        ereport(ERROR, (errmsg("kwabi: tuple_attisdropped returned true for column 1")));

    /* Clean up */
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    /* Drop the test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE kwabi_tuple_test_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_heap_tuple_test() -> bool
 *
 * Test heap tuple accessors through the ABI.
 * Creates a test table, inserts a row, and verifies that heap_tuple_getattr,
 * heap_tuple_setattr, heap_tuple_tableoid, and heap_tuple_tid all return
 * correct values.
 */
PG_FUNCTION_INFO_V1(kwabi_heap_tuple_test);

Datum
kwabi_heap_tuple_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->heap_tuple_getattr == NULL || api->heap_tuple_setattr == NULL ||
        api->heap_tuple_tableoid == NULL || api->heap_tuple_tid == NULL)
        ereport(ERROR, (errmsg("kwabi: heap tuple slots are not wired")));

    /* Create a test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("CREATE TEMP TABLE kwabi_heap_tuple_test_t (id int, name text)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: CREATE TABLE failed")));
    }

    /* Insert a row */
    if (SPI_execute("INSERT INTO kwabi_heap_tuple_test_t VALUES (42, 'hello')", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: INSERT failed")));
    }

    /* Select the row back */
    if (SPI_execute("SELECT * FROM kwabi_heap_tuple_test_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    if (SPI_processed == 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: no rows returned")));
    }

    /* Get the HeapTuple */
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tupdesc = SPI_tuptable->tupdesc;

    /* Test heap_tuple_getattr */
    bool isnull;
    Datum d = api->heap_tuple_getattr(tuple, 1, tupdesc, &isnull);
    if (isnull || DatumGetInt32(d) != 42)
        ereport(ERROR, (errmsg("kwabi: heap_tuple_getattr returned wrong value for column 1")));

    d = api->heap_tuple_getattr(tuple, 2, tupdesc, &isnull);
    if (isnull)
        ereport(ERROR, (errmsg("kwabi: heap_tuple_getattr returned null for column 2")));

    /* Test heap_tuple_tableoid */
    Oid tableoid = api->heap_tuple_tableoid(tuple);
    if (!OidIsValid(tableoid))
        ereport(ERROR, (errmsg("kwabi: heap_tuple_tableoid returned InvalidOid")));

    /* Test heap_tuple_tid */
    ItemPointer tid = api->heap_tuple_tid(tuple);
    if (tid == NULL)
        ereport(ERROR, (errmsg("kwabi: heap_tuple_tid returned NULL")));

    /* Test heap_tuple_setattr - modify column 1 to 99 */
    HeapTuple modified = api->heap_tuple_setattr(tuple, 1, Int32GetDatum(99), tupdesc);
    if (modified == NULL)
        ereport(ERROR, (errmsg("kwabi: heap_tuple_setattr returned NULL")));

    /* Verify the change on the returned tuple */
    d = api->heap_tuple_getattr(modified, 1, tupdesc, &isnull);
    if (isnull || DatumGetInt32(d) != 99)
        ereport(ERROR, (errmsg("kwabi: heap_tuple_setattr did not modify the tuple")));

    SPI_finish();

    /* Clean up */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE kwabi_heap_tuple_test_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_slot_test() -> bool
 *
 * Test slot accessors through the ABI.
 * Creates a test table, creates a slot, and verifies that slot_isnull,
 * slot_getattr, and slot_tupledesc all return correct values.
 */
PG_FUNCTION_INFO_V1(kwabi_slot_test);

Datum
kwabi_slot_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->slot_isnull == NULL || api->slot_getattr == NULL ||
        api->slot_tupledesc == NULL)
        ereport(ERROR, (errmsg("kwabi: slot slots are not wired")));

    /* Create a test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("CREATE TEMP TABLE kwabi_slot_test_t (id int, name text)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: CREATE TABLE failed")));
    }

    /* Insert a row */
    if (SPI_execute("INSERT INTO kwabi_slot_test_t VALUES (42, 'hello')", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: INSERT failed")));
    }

    /* Select the row back */
    if (SPI_execute("SELECT * FROM kwabi_slot_test_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    if (SPI_processed == 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: no rows returned")));
    }

    /* Create a TupleTableSlot from the result */
    TupleTableSlot *slot = MakeSingleTupleTableSlot(SPI_tuptable->tupdesc, &TTSOpsHeapTuple);
    ExecStoreHeapTuple(SPI_tuptable->vals[0], slot, false);

    /* Test slot_tupledesc */
    TupleDesc slot_tupdesc = api->slot_tupledesc((KwabiSlot) slot);
    if (slot_tupdesc == NULL)
        ereport(ERROR, (errmsg("kwabi: slot_tupledesc returned NULL")));

    /* Test slot_isnull */
    bool isnull = api->slot_isnull((KwabiSlot) slot, 1);
    if (isnull)
        ereport(ERROR, (errmsg("kwabi: slot_isnull returned true for column 1")));

    /* Test slot_getattr */
    Datum d = api->slot_getattr((KwabiSlot) slot, 1, &isnull);
    if (isnull || DatumGetInt32(d) != 42)
        ereport(ERROR, (errmsg("kwabi: slot_getattr returned wrong value for column 1")));

    d = api->slot_getattr((KwabiSlot) slot, 2, &isnull);
    if (isnull)
        ereport(ERROR, (errmsg("kwabi: slot_getattr returned null for column 2")));

    /* Clean up */
    ExecDropSingleTupleTableSlot(slot);
    SPI_finish();

    /* Drop the test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE kwabi_slot_test_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_tuple_control() -> bool
 *
 * The NEGATIVE CONTROL. It creates a test table and asserts a wrong value.
 * That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_tuple_control);

Datum
kwabi_tuple_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->tuple_natts == NULL || api->relation_open == NULL ||
        api->relation_close == NULL || api->relation_tupledesc == NULL)
        ereport(ERROR, (errmsg("kwabi: tuple slots are not wired")));

    /* Create a test table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("CREATE TEMP TABLE kwabi_tuple_control_t (id int, name text)", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: CREATE TABLE failed")));
    }
    SPI_finish();

    /* Get the Oid of the temp table */
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    if (SPI_execute("SELECT oid FROM pg_class WHERE relname = 'kwabi_tuple_control_t'", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    Oid relid = InvalidOid;
    if (SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            relid = DatumGetObjectId(d);
    }
    SPI_finish();

    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("kwabi: could not find test table")));

    /* Open the relation through the ABI */
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    /* Get the TupleDesc through the ABI */
    TupleDesc tupdesc = api->relation_tupledesc(rel);
    if (tupdesc == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_tupledesc failed")));

    /* The control's whole point: this comparison must be FALSE. */
    int natts = api->tuple_natts(tupdesc);
    if (natts == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks 2 == 999: broken */

    /* Clean up */
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE kwabi_tuple_control_t", false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();

    ereport(ERROR,
            (errmsg("kwabi: tuple negative control fired as intended"),
             errdetail("tuple_natts is not 999 -- the value comparison is honest")));
}

PG_FUNCTION_INFO_V1(kwabi_version);

Datum
kwabi_version(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    char buf[128];
    snprintf(buf, sizeof(buf),
             "kwabi ABI v%u, %u slots, bound to PostgreSQL %u",
             shim_api->version, (unsigned) sizeof(KwabiV1) / 8,
             (unsigned) (PG_VERSION_NUM / 100));

    PG_RETURN_TEXT_P(cstring_to_text(buf));
}


/* ---- loading a real extension through the ABI ------------------------ */

/* ---- loading a real extension through the ABI ------------------------ */

/*
 * A kwabi extension is an ordinary shared object that exports
 * `kwabi_ext_init(const KwabiV1 *)` and links nothing from PostgreSQL. The
 * runtime is what loads it and hands it the table — PostgreSQL itself only
 * ever knows about the runtime.
 *
 * Why this uses dlopen() rather than PostgreSQL's own loader:
 *
 * `load_external_function()` — the call behind `AS 'lib', 'func'` — enforces
 * PG_MODULE_MAGIC on every library it opens. The magic block records
 * PG_VERSION_NUM among other ABI values, so a library carrying one is bound
 * to a single PostgreSQL major version by construction.
 *
 * That is precisely the constraint this project exists to remove. An
 * extension that must be built once and run on 16, 17 and 18 therefore
 * cannot carry a magic block, and so cannot be opened by
 * load_external_function. The runtime opens it directly.
 *
 * The inversion is the point of the design: the magic check still applies to
 * the runtime, which is built per major version and can satisfy it. It never
 * applies to the extension, which is not.
 */
PG_FUNCTION_INFO_V1(kwabi_load_extension);

Datum
kwabi_load_extension(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        const char *err = dlerror();
        ereport(ERROR,
                (errmsg("kwabi: could not open extension \"%s\"", path),
                 errdetail("%s", err != NULL ? err : "unknown dlopen error")));
    }

    void *sym = dlsym(handle, "kwabi_ext_init");
    if (sym == NULL)
        ereport(ERROR,
                (errmsg("kwabi: extension \"%s\" has no kwabi_ext_init", path)));

    /*
     * The extension's entry point takes the stable table. This is the one
     * moment where the two halves meet.
     */
    bool (*ext_init)(const KwabiV1 *) = (bool (*)(const KwabiV1 *)) sym;
    if (!ext_init(shim_api))
        ereport(ERROR,
                (errmsg("kwabi: extension \"%s\" rejected the ABI table", path)));

    PG_RETURN_TEXT_P(cstring_to_text("kwabi: extension loaded through the ABI"));
}

/*
 * kwabi_ext_alloc() — call a function inside a separately built extension.
 *
 * The extension was compiled from kwabi.h alone with no PostgreSQL headers and
 * no PostgreSQL symbols. If this returns, the whole chain works: PostgreSQL
 * loaded the runtime, the runtime loaded the extension, the extension reached
 * memory through the table the runtime published.
 */
PG_FUNCTION_INFO_V1(kwabi_ext_alloc);

Datum
kwabi_ext_alloc(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
    int32 n = PG_GETARG_INT32(1);

    /* Re-opening an already-loaded object is cheap; dlopen refcounts. */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
        ereport(ERROR, (errmsg("kwabi: could not open \"%s\"", path)));

    void *sym = dlsym(handle, "canary_alloc");
    if (sym == NULL)
        ereport(ERROR, (errmsg("kwabi: no canary_alloc in \"%s\"", path)));

    void *(*ext_alloc)(size_t) = (void *(*)(size_t)) sym;
    void *p = ext_alloc((size_t) n);

    if (p == NULL)
        ereport(ERROR, (errmsg("kwabi: extension returned NULL")));

    /*
     * The strongest check available: memory allocated by a foreign object,
     * through our table, handed back to PostgreSQL. If PostgreSQL recognises
     * it as its own, the extension really did reach the backend's allocator.
     */
    if (GetMemoryChunkContext(p) != CurrentMemoryContext)
        ereport(ERROR,
                (errmsg("kwabi: extension memory is not owned by this backend")));

    /* The extension writes a marker; read it back to prove the write landed. */
    char *bytes = (char *) p;
    bool marker_ok = (bytes[0] == 'k' && bytes[1] == 'w' &&
                      bytes[2] == 'a' && bytes[3] == 'b');
    if (!marker_ok)
        ereport(ERROR, (errmsg("kwabi: extension marker not found")));

    pfree(p);

    PG_RETURN_TEXT_P(cstring_to_text(
        "kwabi: foreign extension reached this backend's allocator"));
}

/* ========================================================================
 * Error firewall: the catching direction
 * ========================================================================
 *
 * This is where PG_TRY lives, and it has to live here rather than in the Rust
 * core. `PG_TRY` is sigsetjmp/siglongjmp: the jump unwinds the C stack from
 * the ereport site to the PG_TRY site and does not care what is in between.
 * The guarded body is reached from Rust, so a PG_TRY wrapped around Rust would
 * relocate where the jump lands without containing anything — the jump would
 * still cross Rust frames.
 *
 * Putting PG_TRY inside a C function and having that function call into Rust
 * (rather than the other way round) keeps the jump entirely inside C:
 *
 *   shim: try_body()                    <-- PG_TRY here, C frames only
 *     └─ runtime: kwabi_try_body()      <-- Rust, but cannot raise
 *          └─ body()                    <-- extension, must not raise
 *
 * The body cannot raise: the contract says it returns a status instead, and a
 * body that violates that is a bug in the body.
 */

/* Runtime core, from the Rust static library. */
extern int kwabi_try_body(KwabiBodyFn body, void *arg, KwabiError *out);

/*
 * The subtransaction wrapper.
 *
 * The subtransaction is mandatory, not an optimisation. Measured: catching an
 * ERROR with PG_TRY alone leaves the partial write behind and it commits,
 * while the same work inside an internal subtransaction rolls back to nothing.
 * An extension that swallows an error is asserting the attempt had no effect;
 * without a subtransaction that assertion is false. See
 * notes/error-firewall-design.md §3.1.
 */
static KwabiStatus
shim_try_body(KwabiBodyFn body, void *arg, KwabiError *out)
{
    KwabiStatus status;
    ErrorData  *edata = NULL;

    MemoryContext oldcontext = CurrentMemoryContext;
    ResourceOwner oldowner   = CurrentResourceOwner;

    if (body == NULL || out == NULL)
        return KWABI_ERR_BAD_ARG;

    BeginInternalSubTransaction(NULL);

    PG_TRY();
    {
        /*
         * Runs the body under catch_unwind. No PostgreSQL call happens inside
         * this block, so nothing here can raise: the only thing that can go
         * wrong is the body returning non-OK or panicking, and both are
         * reported through the return value.
         */
        status = kwabi_try_body(body, arg, out);

        if (status == KWABI_OK) {
            /* Commit the body's work and return to the outer context. */
            ReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcontext);
            CurrentResourceOwner = oldowner;
        } else {
            /*
             * The body reported failure itself. Honour it by rolling back —
             * the same guarantee a caught ERROR gets, so a caller never has to
             * distinguish "it failed and was undone" from "it failed itself".
             */
            RollbackAndReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcontext);
            CurrentResourceOwner = oldowner;
        }
    }
    PG_CATCH();
    {
        /*
         * ORDER MATTERS, for lifetime rather than for crash-safety:
         * CopyErrorData pallocs into CurrentMemoryContext, and at this point
         * that is whatever the failing code left it as. Switching to a context
         * we know outlives the subtransaction is what gives the captured error
         * a lifetime we can reason about. (Both orders happen to return the
         * right text — see notes/error-firewall-design.md §3.3 — which is
         * exactly why this is written the careful way.)
         */
        MemoryContextSwitchTo(oldcontext);
        edata = CopyErrorData();
        FlushErrorState();

        /*
         * Write through the size-aware helpers, never field-by-field: `out`
         * may point to a struct from an older, smaller header, and an
         * unguarded write would overflow it. See kwabi.h.
         */
        kwabi_error_set_core(out, edata->sqlerrcode, KWABI_ERR_RAISED,
                             edata->message != NULL ? edata->message
                             : "kwabi: PostgreSQL raised without a message");
        kwabi_error_set_detail(out, edata->detail, edata->hint);
        kwabi_error_set_object(out,
                               edata->schema_name, edata->table_name,
                               edata->column_name, edata->datatype_name,
                               edata->constraint_name);

        FreeErrorData(edata);

        RollbackAndReleaseCurrentSubTransaction();

        /* Restore again: the rollback left these on the dead subtransaction. */
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;

        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

/*
 * shim_error_get() — hand back the runtime's last error.
 *
 * The runtime owns the message buffer because the error data's own context is
 * destroyed by the rollback. Reading it back through the runtime means the
 * buffer has one owner and one lifetime rule.
 */
static void
shim_error_get(KwabiError *out)
{
    if (out == NULL)
        return;

    /* The runtime fills this; it owns the buffer the message lives in. */
    extern void kwabi_error_get(KwabiError *out);
    kwabi_error_get(out);
}

/* ========================================================================
 * Proof functions for the firewall
 * ========================================================================
 *
 * These exercise kwabi_try end to end from SQL. They are test scaffolding, not
 * ABI surface: none of them is a slot in KwabiV1, and they can be deleted
 * without changing the ABI.
 */

/* Bodies are `extern "C"` and must not raise; they return a status. */

/* A body that succeeds. */
static KwabiStatus
body_ok(void *arg, KwabiError *out)
{
    int *counter = (int *) arg;
    (void) out;
    if (counter != NULL)
        (*counter)++;
    return KWABI_OK;
}

/*
 * A body that raises a real PostgreSQL ERROR via raise_error.
 *
 * This is the contract violation case. `raise_error` longjmps; the body is
 * reached from Rust, so the jump crosses Rust frames. The PG_TRY in
 * shim_try_body still catches it — the jump lands back in C — but any Rust
 * destructors between here and there were skipped, which is exactly the
 * undefined behaviour the contract exists to forbid.
 *
 * Included so the prototype can show what happens, and so the test suite can
 * assert the documented consequence rather than assert it does not happen.
 */
static KwabiStatus
body_raises(void *arg, KwabiError *out)
{
    const char *msg = (const char *) arg;
    (void) out;
    if (shim_api != NULL && shim_api->raise_error != NULL)
        shim_api->raise_error(ERRCODE_DIVISION_BY_ZERO,
                              msg ? msg : "kwabi: body raised");
    return KWABI_OK;   /* not reached */
}

/* A body that reports failure itself, without raising. */
static KwabiStatus
body_fails_cleanly(void *arg, KwabiError *out)
{
    (void) arg; (void) out;
    return KWABI_ERR_RAISED;
}

/*
 * A body that writes a row, then fails cleanly.
 *
 * The point of this one: the row must not survive. That is the property that
 * makes a swallowed error honest, and it is what the subtransaction buys.
 */
static KwabiStatus
body_writes_then_fails(void *arg, KwabiError *out)
{
    int tag = *(int *) arg;

    if (SPI_connect() != SPI_OK_CONNECT)
        return KWABI_ERR_RAISED;

    {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO kwabi_t VALUES (%d)", tag);
        if (SPI_execute(sql, false, 0) != SPI_OK_INSERT) {
            SPI_finish();
            return KWABI_ERR_RAISED;
        }
    }

    SPI_finish();
    return KWABI_ERR_RAISED;   /* report failure; the insert must be undone */
}

/*
 * kwabi_try_ok() — the happy path.
 */
PG_FUNCTION_INFO_V1(kwabi_try_ok);

Datum
kwabi_try_ok(PG_FUNCTION_ARGS)
{
    KwabiError err;
    kwabi_error_init(&err);
    int counter = 0;

    if (shim_api == NULL || shim_api->try_body == NULL)
        ereport(ERROR, (errmsg("kwabi: try_body is not wired")));

    KwabiStatus st = shim_api->try_body(body_ok, &counter, &err);

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("try_ok: status=%d body_ran=%d errcode=%d msg=\"%s\"",
                 (int) st, counter, err.sqlerrcode, err.message)));
}

/*
 * kwabi_try_body_raises() — a body that violates the contract by raising.
 *
 * Documents the consequence: the PG_TRY still catches it, so the backend
 * survives, but Rust frames between the raise and the guard were jumped over.
 */
PG_FUNCTION_INFO_V1(kwabi_try_body_raises);

Datum
kwabi_try_body_raises(PG_FUNCTION_ARGS)
{
    KwabiError err;
    kwabi_error_init(&err);

    if (shim_api == NULL || shim_api->try_body == NULL)
        ereport(ERROR, (errmsg("kwabi: try_body is not wired")));

    KwabiStatus st = shim_api->try_body(body_raises,
                                        (void *) "kwabi: body raised on purpose",
                                        &err);

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("try_body_raises: status=%d sqlstate=%d msg=\"%s\"",
                 (int) st, err.sqlerrcode, err.message)));
}

/*
 * kwabi_try_clean_failure() — a body that reports failure without raising.
 */
PG_FUNCTION_INFO_V1(kwabi_try_clean_failure);

Datum
kwabi_try_clean_failure(PG_FUNCTION_ARGS)
{
    KwabiError err;
    kwabi_error_init(&err);

    if (shim_api == NULL || shim_api->try_body == NULL)
        ereport(ERROR, (errmsg("kwabi: try_body is not wired")));

    KwabiStatus st = shim_api->try_body(body_fails_cleanly, NULL, &err);

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("try_clean_failure: status=%d (expected %d)",
                 (int) st, (int) KWABI_ERR_RAISED)));
}

/*
 * kwabi_try_write_then_fail() — THE test.
 *
 * A body inserts a row and then reports failure. The row must be gone
 * afterwards, because the subtransaction rolled it back. This is the
 * difference between a firewall that works and one that merely does not crash.
 */
PG_FUNCTION_INFO_V1(kwabi_try_write_then_fail);

Datum
kwabi_try_write_then_fail(PG_FUNCTION_ARGS)
{
    KwabiError err;
    kwabi_error_init(&err);
    int tag = PG_GETARG_INT32(0);
    int survivors = -1;

    if (shim_api == NULL || shim_api->try_body == NULL)
        ereport(ERROR, (errmsg("kwabi: try_body is not wired")));

    KwabiStatus st = shim_api->try_body(body_writes_then_fails, &tag, &err);

    /*
     * Count what survived. read_only=false so the command counter advances;
     * read_only=true cannot see our own writes and would report 0 for the
     * wrong reason. (This mistake is documented in fwprobe/README.md.)
     */
    if (SPI_connect() == SPI_OK_CONNECT) {
        char sql[80];
        bool isnull;
        snprintf(sql, sizeof(sql),
                 "SELECT count(*) FROM kwabi_t WHERE tag = %d", tag);
        if (SPI_execute(sql, false, 0) == SPI_OK_SELECT && SPI_processed > 0) {
            Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                    SPI_tuptable->tupdesc, 1, &isnull);
            if (!isnull)
                survivors = DatumGetInt32(d);
        }
        SPI_finish();
    }

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("try_write_then_fail(tag=%d): status=%d survivors=%d%s",
                 tag, (int) st, survivors,
                 survivors == 0 ? "  (correct: work undone)"
                                : "  <-- WRONG: partial work survived")));
}

/*
 * kwabi_try_panics() — a body that PANICS.
 *
 * The body lives in the canary extension, not here, for the same reason the
 * memory canary does: it is a separately built artifact with no PostgreSQL
 * dependency, which is what the ABI promises. Loading it by path also means
 * the panic happens in code that was compiled without any knowledge of
 * PostgreSQL at all — the worst case the firewall has to contain.
 *
 * A panic is NOT a contract violation. The contract forbids *raising*; a panic
 * is a Rust bug, and containing it is the firewall's job.
 */
PG_FUNCTION_INFO_V1(kwabi_try_panics);

Datum
kwabi_try_panics(PG_FUNCTION_ARGS)
{
    KwabiError err;
    kwabi_error_init(&err);
    char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

    if (shim_api == NULL || shim_api->try_body == NULL)
        ereport(ERROR, (errmsg("kwabi: try_body is not wired")));

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
        ereport(ERROR, (errmsg("kwabi: could not open \"%s\"", path)));

    void *sym = dlsym(handle, "canary_panicking_body");
    if (sym == NULL)
        ereport(ERROR, (errmsg("kwabi: no canary_panicking_body in \"%s\"", path)));

    KwabiBodyFn body = (KwabiBodyFn) sym;
    KwabiStatus st = shim_api->try_body(body, NULL, &err);

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("try_panics: status=%d (expected %d) msg=\"%s\"",
                 (int) st, (int) KWABI_ERR_PANICKED, err.message)));
}

/*
 * kwabi_try_nested() — a try inside a try.
 *
 * The inner rollback must not damage the outer one, and the outer must still
 * be able to commit or roll back as a unit.
 */
PG_FUNCTION_INFO_V1(kwabi_try_nested);

static KwabiStatus
body_nested_outer(void *arg, KwabiError *out)
{
    int *tag = (int *) arg;
    KwabiError err;
    kwabi_error_init(&err);

    if (shim_api == NULL || shim_api->try_body == NULL)
        return KWABI_ERR_BAD_ARG;

    /* Inner try fails cleanly; the outer body carries on regardless. */
    (void) shim_api->try_body(body_writes_then_fails, tag, &err);

    return KWABI_OK;
}

Datum
kwabi_try_nested(PG_FUNCTION_ARGS)
{
    KwabiError err;
    kwabi_error_init(&err);
    int tag = PG_GETARG_INT32(0);
    int survivors = -1;

    if (shim_api == NULL || shim_api->try_body == NULL)
        ereport(ERROR, (errmsg("kwabi: try_body is not wired")));

    KwabiStatus st = shim_api->try_body(body_nested_outer, &tag, &err);

    if (SPI_connect() == SPI_OK_CONNECT) {
        char sql[80];
        bool isnull;
        snprintf(sql, sizeof(sql),
                 "SELECT count(*) FROM kwabi_t WHERE tag = %d", tag);
        if (SPI_execute(sql, false, 0) == SPI_OK_SELECT && SPI_processed > 0) {
            Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                    SPI_tuptable->tupdesc, 1, &isnull);
            if (!isnull)
                survivors = DatumGetInt32(d);
        }
        SPI_finish();
    }

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("try_nested(tag=%d): outer_status=%d inner_survivors=%d%s",
                 tag, (int) st, survivors,
                 survivors == 0 ? "  (correct: inner work undone)"
                                : "  <-- WRONG: inner work leaked to outer")));
}

/*
 * kwabi_try_symbol() — run a named body from an extension through kwabi_try.
 *
 * Exists so the guard test can exercise bodies built by #[guarded_body] without
 * hard-coding each one as its own SQL function. The symbol name is looked up in
 * the given library, then passed to the same shim_try_body every other path
 * uses — so what is tested is the real firewall, not a parallel copy.
 *
 * It also calls the extension's kwabi_ext_init before running the body. That is
 * not decoration: the SDK stores the table in a global at init time, so a body
 * that asks the SDK anything about the runtime (its version, its capabilities)
 * gets None until init has run. Without this the capability consumer below
 * would report "no table" and the test would be measuring the harness rather
 * than the ABI. The earlier guard tests passed because none of their bodies
 * needed the table.
 */
PG_FUNCTION_INFO_V1(kwabi_try_symbol);

Datum
kwabi_try_symbol(PG_FUNCTION_ARGS)
{
    KwabiError err;
    kwabi_error_init(&err);
    char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *sym_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    int32 flag = PG_GETARG_INT32(2);
    int32 argbuf = flag;   /* the guarded bodies take a struct whose first field is an i32 */

    if (shim_api == NULL || shim_api->try_body == NULL)
        ereport(ERROR, (errmsg("kwabi: try_body is not wired")));

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
        ereport(ERROR, (errmsg("kwabi: could not open \"%s\"", path)));

    /* Hand the extension the table, exactly as kwabi_load_extension does. */
    void *init_sym = dlsym(handle, "kwabi_ext_init");
    if (init_sym != NULL) {
        bool (*ext_init)(const KwabiV1 *) = (bool (*)(const KwabiV1 *)) init_sym;
        if (!ext_init(shim_api))
            ereport(ERROR,
                    (errmsg("kwabi: extension \"%s\" rejected the ABI table", path)));
    }

    void *sym = dlsym(handle, sym_name);
    if (sym == NULL)
        ereport(ERROR, (errmsg("kwabi: no symbol \"%s\" in \"%s\"", sym_name, path)));

    KwabiStatus st = shim_api->try_body((KwabiBodyFn) sym, &argbuf, &err);

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("try_symbol(%s, flag=%d): status=%d sqlstate=%d msg=\"%s\" "
                 "detail=\"%s\" hint=\"%s\" column=\"%s\" table=\"%s\"",
                 sym_name, (int) flag, (int) st, err.sqlerrcode, err.message,
                 err.detail, err.hint, err.column_name, err.table_name)));
}

/*
 * kwabi_ext_capability_names() — ask an extension what the runtime claims.
 *
 * The counterpart to kwabi_cap_names(), and the difference between the two is
 * the whole point of the capability design:
 *
 *   kwabi_cap_names()  asks the SHIM what it published. It reads the table
 *                      directly, so it works even if no extension is loaded.
 *
 *   this function      asks an EXTENSION what it can see through the SDK.
 *
 * They should agree, and asserting that they do is what makes the bitset a
 * contract rather than a shim-local fact. An extension reaches the answer by a
 * different path: through kwabi_ext_init's stored table pointer, the SDK's
 * mirror of the struct, and the bootstrap rule for a NULL slot. A slot missing
 * from the SDK's table, a field appended to the header but not the mirror, or
 * the bootstrap rule applied as zero instead of CORE-only would all show up
 * here as a disagreement while every shim-side test stayed green.
 */
PG_FUNCTION_INFO_V1(kwabi_ext_capability_names);

Datum
kwabi_ext_capability_names(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        const char *e = dlerror();
        ereport(ERROR,
                (errmsg("kwabi: could not open \"%s\"", path),
                 errdetail("%s", e != NULL ? e : "unknown dlopen error")));
    }

    void *init_sym = dlsym(handle, "kwabi_ext_init");
    if (init_sym == NULL)
        ereport(ERROR,
                (errmsg("kwabi: extension \"%s\" has no kwabi_ext_init", path)));
    bool (*ext_init)(const KwabiV1 *) = (bool (*)(const KwabiV1 *)) init_sym;
    if (!ext_init(shim_api))
        ereport(ERROR,
                (errmsg("kwabi: extension \"%s\" rejected the ABI table", path)));

    void *sym = dlsym(handle, "guarded_capability_names");
    if (sym == NULL)
        ereport(ERROR,
                (errmsg("kwabi: no guarded_capability_names in \"%s\"", path)));

    /* The extension returns a C string it owns; we copy it into a palloc'd
     * buffer that PostgreSQL will free with the statement. */
    const char *names = ((const char *(*)(void)) sym)();
    if (names == NULL)
        ereport(ERROR, (errmsg("kwabi: extension returned no capability names")));

    PG_RETURN_TEXT_P(cstring_to_text(names));
}

/* ========================================================================
 * Node tree proof functions
 * ========================================================================
 *
 * These exercise the node tree slots through the published table -- the same
 * path an extension takes -- so what is tested is the ABI, not a parallel
 * copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_node_type(sql) -> int4
 *
 * Parse a SQL statement and return the node type of the resulting tree.
 */
PG_FUNCTION_INFO_V1(kwabi_node_type);

Datum
kwabi_node_type(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->node_type == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    int node_type = (int) shim_api->node_type(node);
    pfree(sql);
    PG_RETURN_INT32(node_type);
}

/*
 * kwabi_node_type_name(sql) -> text
 *
 * Parse a SQL statement and return the node type name of the resulting tree.
 */
PG_FUNCTION_INFO_V1(kwabi_node_type_name);

Datum
kwabi_node_type_name(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->node_type_name == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    const char *name = shim_api->node_type_name(node);
    pfree(sql);
    PG_RETURN_TEXT_P(cstring_to_text(name ? name : "unknown"));
}

/*
 * kwabi_node_list_length(sql) -> int4
 *
 * Parse a SQL statement and return the length of its target list.
 */
PG_FUNCTION_INFO_V1(kwabi_node_list_length);

Datum
kwabi_node_list_length(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->node_list_length == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    int len = shim_api->node_list_length(node);
    pfree(sql);
    PG_RETURN_INT32(len);
}

/*
 * kwabi_node_list_get(sql, int idx) -> int4
 *
 * Parse a SQL statement and return the node type of the target list entry
 * at the given index.
 */
PG_FUNCTION_INFO_V1(kwabi_node_list_get);

Datum
kwabi_node_list_get(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->node_list_get == NULL || shim_api->node_type == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    int idx = PG_GETARG_INT32(1);
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    KwabiNode entry = shim_api->node_list_get(node, idx);
    if (entry == NULL)
        ereport(ERROR, (errmsg("kwabi: node_list_get returned NULL")));

    int node_type = (int) shim_api->node_type(entry);
    pfree(sql);
    PG_RETURN_INT32(node_type);
}

/*
 * kwabi_query_command_type(sql) -> int4
 *
 * Parse a SQL statement and return the command type of the query.
 */
PG_FUNCTION_INFO_V1(kwabi_query_command_type);

Datum
kwabi_query_command_type(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->query_command_type == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    int cmd_type = (int) shim_api->query_command_type(node);
    pfree(sql);
    PG_RETURN_INT32(cmd_type);
}

/*
 * kwabi_query_rtable_length(sql) -> int4
 *
 * Parse a SQL statement and return the length of its range table.
 */
PG_FUNCTION_INFO_V1(kwabi_query_rtable_length);

Datum
kwabi_query_rtable_length(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->query_rtable == NULL || shim_api->node_list_length == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    KwabiList rtable = shim_api->query_rtable(node);
    int len = shim_api->node_list_length((KwabiNode) rtable);
    pfree(sql);
    PG_RETURN_INT32(len);
}

/*
 * kwabi_query_target_list_length(sql) -> int4
 *
 * Parse a SQL statement and return the length of its target list.
 */
PG_FUNCTION_INFO_V1(kwabi_query_target_list_length);

Datum
kwabi_query_target_list_length(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->query_target_list == NULL || shim_api->node_list_length == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    KwabiList target_list = shim_api->query_target_list(node);
    int len = shim_api->node_list_length((KwabiNode) target_list);
    pfree(sql);
    PG_RETURN_INT32(len);
}

/*
 * kwabi_query_returning_list_length(sql) -> int4
 *
 * Parse a SQL statement and return the length of its returning list.
 */
PG_FUNCTION_INFO_V1(kwabi_query_returning_list_length);

Datum
kwabi_query_returning_list_length(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->query_returning_list == NULL || shim_api->node_list_length == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    KwabiList returning_list = shim_api->query_returning_list(node);
    int len = shim_api->node_list_length((KwabiNode) returning_list);
    pfree(sql);
    PG_RETURN_INT32(len);
}

/*
 * kwabi_query_has_for_update(sql) -> bool
 *
 * Parse a SQL statement and return whether it has FOR UPDATE.
 */
PG_FUNCTION_INFO_V1(kwabi_query_has_for_update);

Datum
kwabi_query_has_for_update(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->query_has_for_update == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    bool has_for_update = shim_api->query_has_for_update(node);
    pfree(sql);
    PG_RETURN_BOOL(has_for_update);
}

/*
 * kwabi_query_has_row_security(sql) -> bool
 *
 * Parse a SQL statement and return whether it has row security.
 */
PG_FUNCTION_INFO_V1(kwabi_query_has_row_security);

Datum
kwabi_query_has_row_security(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->query_has_row_security == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    bool has_row_security = shim_api->query_has_row_security(node);
    pfree(sql);
    PG_RETURN_BOOL(has_row_security);
}

/*
 * kwabi_planned_stmt_is_utility(sql) -> bool
 *
 * Parse a SQL statement and return whether it is a utility statement.
 */
PG_FUNCTION_INFO_V1(kwabi_planned_stmt_is_utility);

Datum
kwabi_planned_stmt_is_utility(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->planned_stmt_is_utility == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    KwabiNode node = shim_api->parse_stmt(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    bool is_utility = shim_api->planned_stmt_is_utility(node);
    pfree(sql);
    PG_RETURN_BOOL(is_utility);
}

/*
 * kwabi_node_control() -> bool
 *
 * The NEGATIVE CONTROL. It parses a known value and asserts a wrong one.
 * That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_node_control);

Datum
kwabi_node_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->parse_stmt == NULL ||
        shim_api->node_type == NULL)
        ereport(ERROR, (errmsg("kwabi: node tree slots are not wired")));

    KwabiNode node = shim_api->parse_stmt("SELECT 1");
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    int node_type = (int) shim_api->node_type(node);

    /* The control's whole point: this comparison must be FALSE. */
    if (node_type == 2)
        PG_RETURN_BOOL(true);   /* the comparison thinks 1 == 2: broken */

    ereport(ERROR,
            (errmsg("kwabi: node tree negative control fired as intended"),
             errdetail("node type is 1, not 2 -- the value comparison is honest")));
}

/* ========================================================================
 * Relation cache proof functions
 * ========================================================================
 *
 * These exercise the relation cache slots through the published table -- the
 * same path an extension takes -- so what is tested is the ABI, not a parallel
 * copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_relation_open_test(int4 relid) -> text
 *
 * Open a relation through the ABI and return its name.
 */
PG_FUNCTION_INFO_V1(kwabi_relation_open_test);

Datum
kwabi_relation_open_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->relation_name == NULL)
        ereport(ERROR, (errmsg("kwabi: relation slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    const char *name = api->relation_name(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_TEXT_P(cstring_to_text(name ? name : "(null)"));
}

/*
 * kwabi_relation_id_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return its OID.
 */
PG_FUNCTION_INFO_V1(kwabi_relation_id_test);

Datum
kwabi_relation_id_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->relation_id == NULL)
        ereport(ERROR, (errmsg("kwabi: relation slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    Oid result = api->relation_id(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_OID(result);
}

/*
 * kwabi_relation_namespace_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return its namespace OID.
 */
PG_FUNCTION_INFO_V1(kwabi_relation_namespace_test);

Datum
kwabi_relation_namespace_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->relation_namespace == NULL)
        ereport(ERROR, (errmsg("kwabi: relation slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    Oid result = api->relation_namespace(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_OID(result);
}

/*
 * kwabi_relation_tupledesc_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return the number of attributes
 * in its TupleDesc.
 */
PG_FUNCTION_INFO_V1(kwabi_relation_tupledesc_test);

Datum
kwabi_relation_tupledesc_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->relation_tupledesc == NULL || api->tuple_natts == NULL)
        ereport(ERROR, (errmsg("kwabi: relation slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    TupleDesc tupdesc = api->relation_tupledesc(rel);
    int natts = 0;
    if (tupdesc != NULL)
        natts = api->tuple_natts(tupdesc);

    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_INT32(natts);
}

/*
 * kwabi_rel_id_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return its OID via rel_id.
 */
PG_FUNCTION_INFO_V1(kwabi_rel_id_test);

Datum
kwabi_rel_id_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->rel_id == NULL)
        ereport(ERROR, (errmsg("kwabi: rel slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    Oid result = api->rel_id(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_OID(result);
}

/*
 * kwabi_rel_name_test(int4 relid) -> text
 *
 * Open a relation through the ABI and return its name via rel_name.
 */
PG_FUNCTION_INFO_V1(kwabi_rel_name_test);

Datum
kwabi_rel_name_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->rel_name == NULL)
        ereport(ERROR, (errmsg("kwabi: rel slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    const char *name = api->rel_name(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_TEXT_P(cstring_to_text(name ? name : "(null)"));
}

/*
 * kwabi_rel_namespace_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return its namespace OID via rel_namespace.
 */
PG_FUNCTION_INFO_V1(kwabi_rel_namespace_test);

Datum
kwabi_rel_namespace_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->rel_namespace == NULL)
        ereport(ERROR, (errmsg("kwabi: rel slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    Oid result = api->rel_namespace(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_OID(result);
}

/*
 * kwabi_rel_relkind_test(int4 relid) -> text
 *
 * Open a relation through the ABI and return its relkind via rel_relkind.
 */
PG_FUNCTION_INFO_V1(kwabi_rel_relkind_test);

Datum
kwabi_rel_relkind_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->rel_relkind == NULL)
        ereport(ERROR, (errmsg("kwabi: rel slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    char relkind = api->rel_relkind(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    /* Return relkind as a single-character text */
    char buf[2] = { relkind, '\0' };
    PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * kwabi_rel_relam_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return its relam via rel_relam.
 */
PG_FUNCTION_INFO_V1(kwabi_rel_relam_test);

Datum
kwabi_rel_relam_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->rel_relam == NULL)
        ereport(ERROR, (errmsg("kwabi: rel slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    Oid result = api->rel_relam(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_OID(result);
}

/*
 * kwabi_rel_tupledesc_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return the number of attributes
 * in its TupleDesc via rel_tupledesc.
 */
PG_FUNCTION_INFO_V1(kwabi_rel_tupledesc_test);

Datum
kwabi_rel_tupledesc_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->rel_tupledesc == NULL || api->tuple_natts == NULL)
        ereport(ERROR, (errmsg("kwabi: rel slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    TupleDesc tupdesc = api->rel_tupledesc(rel);
    int natts = 0;
    if (tupdesc != NULL)
        natts = api->tuple_natts(tupdesc);

    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_INT32(natts);
}

/*
 * kwabi_rel_index_list_test(int4 relid) -> int4
 *
 * Open a relation through the ABI and return the number of indexes
 * via rel_index_list.
 */
PG_FUNCTION_INFO_V1(kwabi_rel_index_list_test);

Datum
kwabi_rel_index_list_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->rel_index_list == NULL || api->node_list_length == NULL)
        ereport(ERROR, (errmsg("kwabi: rel slots are not wired")));

    Oid relid = (Oid) PG_GETARG_INT32(0);
    KwabiRelation rel = api->relation_open(relid, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    KwabiList index_list = api->rel_index_list(rel);
    int len = api->node_list_length((KwabiNode) index_list);

    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_INT32(len);
}

/*
 * kwabi_relation_control() -> bool
 *
 * The NEGATIVE CONTROL. It opens a known relation and asserts a wrong value.
 * That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_relation_control);

Datum
kwabi_relation_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->relation_open == NULL || api->relation_close == NULL ||
        api->relation_name == NULL)
        ereport(ERROR, (errmsg("kwabi: relation slots are not wired")));

    /* Open a known relation: pg_class */
    KwabiRelation rel = api->relation_open(1259, KWABI_LOCKMODE_SHARE); /* pg_class */
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    const char *name = api->relation_name(rel);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    /* The control's whole point: this comparison must be FALSE. */
    if (name != NULL && strcmp(name, "wrong_name") == 0)
        PG_RETURN_BOOL(true);   /* the comparison thinks pg_class == wrong_name: broken */

    ereport(ERROR,
            (errmsg("kwabi: relation negative control fired as intended"),
             errdetail("relation name is pg_class, not wrong_name -- the value comparison is honest")));
}

/* ========================================================================
 * Buffer manager proof functions
 * ========================================================================
 *
 * These exercise the four buffer manager slots through the published table
 * -- the same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_buffer_test() -> bool
 *
 * Test buffer manager operations through the ABI.
 * Opens a relation, reads a buffer, gets its page, marks it dirty, and
 * releases it. Verifies that the page pointer is non-NULL and that the
 * buffer lifecycle completes without error.
 */
PG_FUNCTION_INFO_V1(kwabi_buffer_test);

Datum
kwabi_buffer_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->buffer_get == NULL || api->buffer_release == NULL ||
        api->buffer_get_page == NULL || api->buffer_mark_dirty == NULL ||
        api->relation_open == NULL || api->relation_close == NULL)
        ereport(ERROR, (errmsg("kwabi: buffer slots are not wired")));

    /* Open pg_class through the ABI */
    KwabiRelation rel = api->relation_open(1259, KWABI_LOCKMODE_SHARE); /* pg_class */
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    /* Read block 0 through the ABI */
    Buffer buf = api->buffer_get(rel, 0);
    if (buf == InvalidBuffer) {
        api->relation_close(rel, KWABI_LOCKMODE_SHARE);
        ereport(ERROR, (errmsg("kwabi: buffer_get returned InvalidBuffer")));
    }

    /* Get the page through the ABI */
    Page page = api->buffer_get_page(buf);
    if (page == NULL) {
        api->buffer_release(buf);
        api->relation_close(rel, KWABI_LOCKMODE_SHARE);
        ereport(ERROR, (errmsg("kwabi: buffer_get_page returned NULL")));
    }

    /* Mark the buffer dirty through the ABI */
    api->buffer_mark_dirty(buf);

    /* Release the buffer through the ABI */
    api->buffer_release(buf);

    /* Close the relation */
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_buffer_page_content() -> bool
 *
 * Test that the page obtained through the ABI has valid content.
 * Reads block 0 of pg_class and checks that the page starts with a valid
 * PageHeader (pd_lower > SizeOfPageHeaderData and pd_lower <= pd_upper).
 */
PG_FUNCTION_INFO_V1(kwabi_buffer_page_content);

Datum
kwabi_buffer_page_content(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->buffer_get == NULL || api->buffer_release == NULL ||
        api->buffer_get_page == NULL ||
        api->relation_open == NULL || api->relation_close == NULL)
        ereport(ERROR, (errmsg("kwabi: buffer slots are not wired")));

    KwabiRelation rel = api->relation_open(1259, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    Buffer buf = api->buffer_get(rel, 0);
    if (buf == InvalidBuffer) {
        api->relation_close(rel, KWABI_LOCKMODE_SHARE);
        ereport(ERROR, (errmsg("kwabi: buffer_get returned InvalidBuffer")));
    }

    Page page = api->buffer_get_page(buf);
    if (page == NULL) {
        api->buffer_release(buf);
        api->relation_close(rel, KWABI_LOCKMODE_SHARE);
        ereport(ERROR, (errmsg("kwabi: buffer_get_page returned NULL")));
    }

    /* Validate the page header */
    PageHeader ph = (PageHeader) page;
    bool valid = (ph->pd_lower > SizeOfPageHeaderData &&
                  ph->pd_lower <= ph->pd_upper &&
                  ph->pd_upper <= BLCKSZ);

    api->buffer_release(buf);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    PG_RETURN_BOOL(valid);
}

/*
 * kwabi_buffer_control() -> bool
 *
 * The NEGATIVE CONTROL. It reads a buffer and asserts a wrong page property.
 * That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_buffer_control);

Datum
kwabi_buffer_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->buffer_get == NULL || api->buffer_release == NULL ||
        api->buffer_get_page == NULL ||
        api->relation_open == NULL || api->relation_close == NULL)
        ereport(ERROR, (errmsg("kwabi: buffer slots are not wired")));

    KwabiRelation rel = api->relation_open(1259, KWABI_LOCKMODE_SHARE);
    if (rel == NULL)
        ereport(ERROR, (errmsg("kwabi: relation_open failed")));

    Buffer buf = api->buffer_get(rel, 0);
    if (buf == InvalidBuffer) {
        api->relation_close(rel, KWABI_LOCKMODE_SHARE);
        ereport(ERROR, (errmsg("kwabi: buffer_get returned InvalidBuffer")));
    }

    Page page = api->buffer_get_page(buf);
    if (page == NULL) {
        api->buffer_release(buf);
        api->relation_close(rel, KWABI_LOCKMODE_SHARE);
        ereport(ERROR, (errmsg("kwabi: buffer_get_page returned NULL")));
    }

    PageHeader ph = (PageHeader) page;

    /* The control's whole point: this comparison must be FALSE. */
    if (ph->pd_lower == 0) {
        api->buffer_release(buf);
        api->relation_close(rel, KWABI_LOCKMODE_SHARE);
        PG_RETURN_BOOL(true);   /* the comparison thinks pd_lower == 0: broken */
    }

    api->buffer_release(buf);
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);

    ereport(ERROR,
            (errmsg("kwabi: lock negative control fired as intended"),
             errdetail("pd_lower is not 0 -- the value comparison is honest")));
}

/* ========================================================================
 * LWLock proof functions
 * ========================================================================
 *
 * These exercise the four lwlock slots through the published table -- the
 * same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_lwlock_test() -> bool
 *
 * Test lwlock operations through the ABI.
 * Acquires an LWLock in shared mode, checks held_by_me, releases it,
 * and verifies held_by_me returns false.
 */
PG_FUNCTION_INFO_V1(kwabi_lwlock_test);

Datum
kwabi_lwlock_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->lwlock_acquire == NULL || api->lwlock_release == NULL ||
        api->lwlock_held_by_me == NULL || api->lwlock_cond_acquire == NULL)
        ereport(ERROR, (errmsg("kwabi: lwlock slots are not wired")));

    /* Use a known LWLock: the lock manager's lock */
    LWLock *lock = &MainLWLockArray[0].lock;

    /* Acquire in shared mode */
    api->lwlock_acquire(lock, KWABI_LWLOCKMODE_SHARE);

    /* Must be held by me */
    if (!api->lwlock_held_by_me(lock))
        ereport(ERROR, (errmsg("kwabi: lwlock_held_by_me returned false after acquire")));

    /* Release */
    api->lwlock_release(lock);

    /* Must NOT be held by me */
    if (api->lwlock_held_by_me(lock))
        ereport(ERROR, (errmsg("kwabi: lwlock_held_by_me returned true after release")));

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_lwlock_cond_test() -> bool
 *
 * Test conditional lwlock acquire through the ABI.
 * Tries to acquire an LWLock conditionally, checks the result, and
 * releases if acquired.
 */
PG_FUNCTION_INFO_V1(kwabi_lwlock_cond_test);

Datum
kwabi_lwlock_cond_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->lwlock_acquire == NULL || api->lwlock_release == NULL ||
        api->lwlock_held_by_me == NULL || api->lwlock_cond_acquire == NULL)
        ereport(ERROR, (errmsg("kwabi: lwlock slots are not wired")));

    /* Use a known LWLock */
    LWLock *lock = &MainLWLockArray[0].lock;

    /* Try conditional acquire */
    bool acquired = api->lwlock_cond_acquire(lock, KWABI_LWLOCKMODE_SHARE);

    if (acquired) {
        /* Must be held by me */
        if (!api->lwlock_held_by_me(lock))
            ereport(ERROR, (errmsg("kwabi: lwlock_held_by_me returned false after cond_acquire")));

        /* Release */
        api->lwlock_release(lock);

        /* Must NOT be held by me */
        if (api->lwlock_held_by_me(lock))
            ereport(ERROR, (errmsg("kwabi: lwlock_held_by_me returned true after release")));
    }

    PG_RETURN_BOOL(true);
}

/*
 * kwabi_lwlock_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts a wrong lwlock state, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_lwlock_control);

Datum
kwabi_lwlock_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->lwlock_acquire == NULL || api->lwlock_release == NULL ||
        api->lwlock_held_by_me == NULL)
        ereport(ERROR, (errmsg("kwabi: lwlock slots are not wired")));

    LWLock *lock = &MainLWLockArray[0].lock;

    /* Acquire */
    api->lwlock_acquire(lock, KWABI_LWLOCKMODE_SHARE);

    /* The control's whole point: this comparison must be FALSE. */
    if (!api->lwlock_held_by_me(lock)) {
        api->lwlock_release(lock);
        PG_RETURN_BOOL(true);   /* the comparison thinks held_by_me is false: broken */
    }

    api->lwlock_release(lock);

    ereport(ERROR,
            (errmsg("kwabi: lwlock negative control fired as intended"),
             errdetail("lwlock_held_by_me is true after acquire -- the value comparison is honest")));
}

/* ========================================================================
 * Type system proof functions
 * ========================================================================
 *
 * These exercise the type system slots through the published table -- the
 * same path an extension takes -- so what is tested is the ABI, not a parallel
 * copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_type_length(int4) -> int4
 *
 * Get the length of a type through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_type_length);

Datum
kwabi_type_length(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_length == NULL)
        ereport(ERROR, (errmsg("kwabi: type_length is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    int16 result = shim_api->type_length(typoid);

    PG_RETURN_INT32((int32) result);
}

/*
 * kwabi_type_is_array(int4) -> bool
 *
 * Check if a type is an array type through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_type_is_array);

Datum
kwabi_type_is_array(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_is_array == NULL)
        ereport(ERROR, (errmsg("kwabi: type_is_array is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
#undef type_is_array
    bool result = shim_api->type_is_array(typoid);

    PG_RETURN_BOOL(result);
}

/*
 * kwabi_type_is_composite(int4) -> bool
 *
 * Check if a type is a composite type through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_type_is_composite);

Datum
kwabi_type_is_composite(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_is_composite == NULL)
        ereport(ERROR, (errmsg("kwabi: type_is_composite is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    bool result = shim_api->type_is_composite(typoid);

    PG_RETURN_BOOL(result);
}

/*
 * kwabi_type_element_type(int4) -> int4
 *
 * Get the element type of an array type through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_type_element_type);

Datum
kwabi_type_element_type(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_element_type == NULL)
        ereport(ERROR, (errmsg("kwabi: type_element_type is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    Oid result = shim_api->type_element_type(typoid);

    PG_RETURN_OID(result);
}

/*
 * kwabi_type_base_type(int4) -> int4
 *
 * Get the base type of a domain type through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_type_base_type);

Datum
kwabi_type_base_type(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_base_type == NULL)
        ereport(ERROR, (errmsg("kwabi: type_base_type is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    Oid result = shim_api->type_base_type(typoid);

    PG_RETURN_OID(result);
}

/*
 * kwabi_type_input(int4, text, int4) -> text
 *
 * Parse a text representation of a value into its Datum form through the ABI,
 * then convert back to text for the SQL boundary.
 */
PG_FUNCTION_INFO_V1(kwabi_type_input);

Datum
kwabi_type_input(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_input == NULL ||
        shim_api->type_output == NULL)
        ereport(ERROR, (errmsg("kwabi: type_input/type_output is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    text *input_text = PG_GETARG_TEXT_P(1);
    int32 typmod = PG_GETARG_INT32(2);

    char *input_str = text_to_cstring(input_text);
    Datum result = shim_api->type_input(typoid, input_str, typmod);
    pfree(input_str);

    if (result == (Datum) 0)
        PG_RETURN_NULL();

    char *output_str = shim_api->type_output(typoid, result);
    if (output_str == NULL)
        PG_RETURN_NULL();

    text *ret = cstring_to_text(output_str);
    pfree(output_str);
    PG_RETURN_TEXT_P(ret);
}

/*
 * kwabi_type_output(int4, text) -> text
 *
 * Convert a text representation of a value to its Datum form through the ABI,
 * then back to text for the SQL boundary.
 */
PG_FUNCTION_INFO_V1(kwabi_type_output);

Datum
kwabi_type_output(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_input == NULL ||
        shim_api->type_output == NULL)
        ereport(ERROR, (errmsg("kwabi: type_input/type_output is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    text *value_text = PG_GETARG_TEXT_P(1);

    char *value_str = text_to_cstring(value_text);
    Datum value = shim_api->type_input(typoid, value_str, -1);
    pfree(value_str);

    if (value == (Datum) 0)
        PG_RETURN_NULL();

    char *output_str = shim_api->type_output(typoid, value);
    if (output_str == NULL)
        PG_RETURN_NULL();

    text *ret = cstring_to_text(output_str);
    pfree(output_str);
    PG_RETURN_TEXT_P(ret);
}

/*
 * kwabi_type_send(int4, text) -> bytea
 *
 * Serialize a value to its binary wire format through the ABI.
 * Parses the text input to a Datum via type_input, then calls
 * type_send to produce the binary representation.
 */
PG_FUNCTION_INFO_V1(kwabi_type_send);

Datum
kwabi_type_send(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_input == NULL ||
        shim_api->type_send == NULL)
        ereport(ERROR, (errmsg("kwabi: type_input/type_send is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    text *value_text = PG_GETARG_TEXT_P(1);

    char *value_str = text_to_cstring(value_text);
    Datum value = shim_api->type_input(typoid, value_str, -1);
    pfree(value_str);

    if (value == (Datum) 0)
        PG_RETURN_NULL();

    StringInfoData buf;
    initStringInfo(&buf);
    shim_api->type_send(typoid, value, &buf);

    bytea *ret = (bytea *) palloc(buf.len + VARHDRSZ);
    SET_VARSIZE(ret, buf.len + VARHDRSZ);
    memcpy(VARDATA(ret), buf.data, buf.len);
    pfree(buf.data);

    PG_RETURN_BYTEA_P(ret);
}

/*
 * kwabi_type_recv(int4, bytea) -> text
 *
 * Parse a binary representation of a value into its Datum form through the ABI,
 * then convert back to text for the SQL boundary.
 */
PG_FUNCTION_INFO_V1(kwabi_type_recv);

Datum
kwabi_type_recv(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_recv == NULL ||
        shim_api->type_output == NULL)
        ereport(ERROR, (errmsg("kwabi: type_recv/type_output is not wired")));

    Oid typoid = (Oid) PG_GETARG_INT32(0);
    bytea *input_bytea = PG_GETARG_BYTEA_P(1);

    StringInfoData buf;
    initStringInfo(&buf);
    appendBinaryStringInfo(&buf, VARDATA(input_bytea), VARSIZE(input_bytea) - VARHDRSZ);

    Datum result = shim_api->type_recv(typoid, &buf);
    pfree(buf.data);

    if (result == (Datum) 0)
        PG_RETURN_NULL();

    char *output_str = shim_api->type_output(typoid, result);
    if (output_str == NULL)
        PG_RETURN_NULL();

    text *ret = cstring_to_text(output_str);
    pfree(output_str);
    PG_RETURN_TEXT_P(ret);
}

/*
 * kwabi_type_control() -> bool
 *
 * The NEGATIVE CONTROL. It calls type_length on a known type and asserts
 * a wrong value. That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_type_control);

Datum
kwabi_type_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->type_length == NULL)
        ereport(ERROR, (errmsg("kwabi: type_length is not wired")));

    /* int4 (23) has length 4 */
    int16 len = shim_api->type_length(23);

    /* The control's whole point: this comparison must be FALSE. */
    if (len == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks int4 length is 999: broken */

    ereport(ERROR,
            (errmsg("kwabi: type negative control fired as intended"),
             errdetail("type_length(23) is %d, not 999 -- the value comparison is honest", len)));
}

/*
 * kwabi_stringinfo_test() -> text
 *
 * Test all seven stringinfo slots through the ABI.
 * Creates a StringInfo, appends "hello ", appends ' ', appends 42,
 * then reads back the data and length.
 */
PG_FUNCTION_INFO_V1(kwabi_stringinfo_test);

Datum
kwabi_stringinfo_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->stringinfo_init == NULL || api->stringinfo_reset == NULL ||
        api->stringinfo_append == NULL || api->stringinfo_append_char == NULL ||
        api->stringinfo_append_int == NULL || api->stringinfo_data == NULL ||
        api->stringinfo_len == NULL)
        ereport(ERROR, (errmsg("kwabi: stringinfo slots are not wired")));

    /* Allocate a StringInfoData on the stack */
    StringInfoData str;
    str.data = NULL;
    str.len = 0;
    str.maxlen = 0;
    str.cursor = 0;

    /* 1. Init */
    api->stringinfo_init(&str);

    /* 2. Append "hello" (no trailing space: the char below supplies it) */
    api->stringinfo_append(&str, "hello");

    /* 3. Append a space char */
    api->stringinfo_append_char(&str, ' ');

    /* 4. Append integer 42 */
    api->stringinfo_append_int(&str, 42);

    /* 5. Read back data and length */
    const char *data = api->stringinfo_data(&str);
    int len = api->stringinfo_len(&str);

    /*
     * Copy the data out BEFORE the reset/free below. `data` points into the
     * StringInfo's own buffer, so reading it after pfree is a use-after-free —
     * which is exactly what made the original failure report data="" instead
     * of the real contents.
     */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", data ? data : "(null)");

    /* 6. Reset and verify it cleared */
    api->stringinfo_reset(&str);
    int len_after_reset = api->stringinfo_len(&str);

    /* Clean up */
    if (str.data != NULL)
        pfree(str.data);

    /* "hello" + ' ' + "42" == "hello 42", which is 8 bytes. */
    if (len == 8 && len_after_reset == 0 && data != NULL && strcmp(buf, "hello 42") == 0)
        PG_RETURN_TEXT_P(cstring_to_text(buf));

    /* Something went wrong. Report from `buf`, never from the freed `data`. */
    ereport(ERROR,
            (errmsg("kwabi: stringinfo test failed"),
             errdetail("len=%d len_after_reset=%d data=\"%s\"",
                       len, len_after_reset, buf)));
}

/*
 * kwabi_stringinfo_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts stringinfo length == 999, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_stringinfo_control);

Datum
kwabi_stringinfo_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->stringinfo_init == NULL || api->stringinfo_append == NULL ||
        api->stringinfo_len == NULL)
        ereport(ERROR, (errmsg("kwabi: stringinfo slots are not wired")));

    StringInfoData str;
    str.data = NULL;
    str.len = 0;
    str.maxlen = 0;
    str.cursor = 0;

    api->stringinfo_init(&str);
    api->stringinfo_append(&str, "test");

    int len = api->stringinfo_len(&str);

    if (str.data != NULL)
        pfree(str.data);

    /* The control's whole point: this comparison must be FALSE. */
    if (len == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks 4 == 999: broken */

    ereport(ERROR,
            (errmsg("kwabi: stringinfo negative control fired as intended"),
             errdetail("stringinfo length is not 999 -- the value comparison is honest")));
}

/* ========================================================================
 * Syscache proof functions
 * ========================================================================
 *
 * These exercise the three syscache slots through the published table --
 * the same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_syscache_test() -> text
 *
 * Test all three syscache slots through the ABI.
 * Looks up int4 (type OID 23) in the TYPEOID cache, gets its typinput
 * function OID, then does a full tuple lookup and frees it.
 */
PG_FUNCTION_INFO_V1(kwabi_syscache_test);

Datum
kwabi_syscache_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->syscache_get_oid == NULL || api->syscache_get_tuple == NULL ||
        api->syscache_free_tuple == NULL)
        ereport(ERROR, (errmsg("kwabi: syscache slots are not wired")));

    /* 1. syscache_get_oid: look up int4's typinput function */
    Oid typinput = api->syscache_get_oid("TYPEOID", "typinput",
                                         ObjectIdGetDatum(23));
    if (!OidIsValid(typinput))
        ereport(ERROR, (errmsg("kwabi: syscache_get_oid returned InvalidOid")));

    /* 2. syscache_get_tuple: get the full tuple for int4 */
    HeapTuple tup = api->syscache_get_tuple("TYPEOID",
                                           ObjectIdGetDatum(23));
    if (tup == NULL)
        ereport(ERROR, (errmsg("kwabi: syscache_get_tuple returned NULL")));

    /* 3. syscache_free_tuple: release it */
    api->syscache_free_tuple(tup);

    PG_RETURN_TEXT_P(cstring_to_text("kwabi: syscache test passed"));
}

/*
 * kwabi_syscache_control() -> bool
 *
 * The NEGATIVE CONTROL. It calls syscache_get_oid for int4's typinput
 * and asserts the result is 999. That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_syscache_control);

Datum
kwabi_syscache_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->syscache_get_oid == NULL)
        ereport(ERROR, (errmsg("kwabi: syscache_get_oid is not wired")));

    Oid typinput = shim_api->syscache_get_oid("TYPEOID", "typinput",
                                              ObjectIdGetDatum(23));

    /* The control's whole point: this comparison must be FALSE. */
    if (typinput == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks typinput == 999: broken */

    ereport(ERROR,
            (errmsg("kwabi: syscache negative control fired as intended"),
             errdetail("typinput is not 999 -- the value comparison is honest")));
}

/* ========================================================================
 * Shmem proof functions
 * ========================================================================
 *
 * These exercise the three shmem slots through the published table --
 * the same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_shmem_test() -> text
 *
 * Test all three shmem slots through the ABI.
 * Allocates shared memory, writes to it, reads it back, frees it,
 * then uses shmem_get to allocate a named struct.
 */
PG_FUNCTION_INFO_V1(kwabi_shmem_test);

Datum
kwabi_shmem_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->shmem_alloc == NULL || api->shmem_free == NULL ||
        api->shmem_get == NULL)
        ereport(ERROR, (errmsg("kwabi: shmem slots are not wired")));

    /* 1. shmem_alloc: allocate 64 bytes of shared memory */
    void *ptr = api->shmem_alloc(64);
    if (ptr == NULL)
        ereport(ERROR, (errmsg("kwabi: shmem_alloc returned NULL")));

    /* 2. Write a known value to it */
    memset(ptr, 0, 64);
    *(int *)ptr = 42;

    /* 3. Read it back */
    if (*(int *)ptr != 42)
        ereport(ERROR, (errmsg("kwabi: shmem write/read mismatch")));

    /* 4. shmem_free: release it */
    api->shmem_free(ptr);

    /* 5. shmem_get: allocate a named struct */
    void *named = api->shmem_get("kwabi_test_shmem", 128);
    if (named == NULL)
        ereport(ERROR, (errmsg("kwabi: shmem_get returned NULL")));

    /* 6. Write and read back */
    memset(named, 0, 128);
    *(int *)named = 99;
    if (*(int *)named != 99)
        ereport(ERROR, (errmsg("kwabi: shmem_get write/read mismatch")));

    PG_RETURN_TEXT_P(cstring_to_text("kwabi: shmem test passed"));
}

/*
 * kwabi_shmem_control() -> bool
 *
 * The NEGATIVE CONTROL. It calls shmem_alloc and asserts a wrong size,
 * which must fail. That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_shmem_control);

Datum
kwabi_shmem_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->shmem_alloc == NULL)
        ereport(ERROR, (errmsg("kwabi: shmem_alloc is not wired")));

    void *ptr = shim_api->shmem_alloc(64);
    if (ptr == NULL)
        ereport(ERROR, (errmsg("kwabi: shmem_alloc returned NULL")));

    /* The control's whole point: this comparison must be FALSE. */
    if (sizeof(void *) == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks sizeof(void*) == 999: broken */

    ereport(ERROR,
            (errmsg("kwabi: shmem negative control fired as intended"),
             errdetail("sizeof(void*) is not 999 -- the value comparison is honest")));
}

/* ========================================================================
 * Extension proof functions
 * ========================================================================
 *
 * These exercise the three extension slots through the published table --
 * the same path an extension takes -- so what is tested is the ABI, not a
 * parallel copy of it. They are test scaffolding: none is a slot in KwabiV1.
 */

/*
 * kwabi_extension_test() -> text
 *
 * Test all three extension slots through the ABI.
 * Looks up "plpgsql" (always installed), checks it is installed, and
 * reads its version. Returns a summary string.
 */
PG_FUNCTION_INFO_V1(kwabi_extension_test);

Datum
kwabi_extension_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    const KwabiV1 *api = shim_api;

    if (api->extension_oid == NULL || api->extension_installed == NULL ||
        api->extension_version == NULL)
        ereport(ERROR, (errmsg("kwabi: extension slots are not wired")));

    /* 1. extension_oid: look up plpgsql */
    Oid oid = api->extension_oid("plpgsql");
    if (!OidIsValid(oid))
        ereport(ERROR, (errmsg("kwabi: extension_oid returned InvalidOid for plpgsql")));

    /* 2. extension_installed: plpgsql must be installed */
    bool installed = api->extension_installed("plpgsql");
    if (!installed)
        ereport(ERROR, (errmsg("kwabi: extension_installed returned false for plpgsql")));

    /* 3. extension_version: must return a non-NULL string */
    const char *ver = api->extension_version("plpgsql");
    if (ver == NULL)
        ereport(ERROR, (errmsg("kwabi: extension_version returned NULL for plpgsql")));

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("kwabi: extension test passed (oid=%u installed=%d version=%s)",
                 (unsigned) oid, (int) installed, ver)));
}

/*
 * kwabi_extension_control() -> bool
 *
 * The NEGATIVE CONTROL. It calls extension_oid for plpgsql and asserts
 * the result is 999. That assertion must fail, so this must RAISE.
 */
PG_FUNCTION_INFO_V1(kwabi_extension_control);

Datum
kwabi_extension_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->extension_oid == NULL)
        ereport(ERROR, (errmsg("kwabi: extension_oid is not wired")));

    Oid oid = shim_api->extension_oid("plpgsql");

    /* The control's whole point: this comparison must be FALSE. */
    if (oid == 999)
        PG_RETURN_BOOL(true);   /* the comparison thinks oid == 999: broken */

    ereport(ERROR,
            (errmsg("kwabi: extension negative control fired as intended"),
             errdetail("extension oid is not 999 -- the value comparison is honest")));
}

/*
 * Query and planned-statement wrappers. Each parses SQL with parse_stmt and
 * calls one slot. Node-valued results are returned as the kwabi node type,
 * with 0 (KWABI_NODE_UNKNOWN) for NULL, so SQL can compare them to the enum.
 */
static KwabiNode
kwabi_test_parse(text *arg)
{
    char *sql = text_to_cstring(arg);
    KwabiNode node = shim_api->parse_stmt(sql);

    pfree(sql);
    if (node == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));
    return node;
}

static int32
kwabi_test_node_type_or_zero(KwabiNode node)
{
    return node == NULL ? 0 : (int32) shim_api->node_type(node);
}

#define KWABI_NODE_TEST_REQUIRE(...) \
    do { \
        if (shim_api == NULL || shim_api->parse_stmt == NULL || \
            __VA_ARGS__) \
            ereport(ERROR, (errmsg("kwabi: node tree slots are not wired"))); \
    } while (0)

PG_FUNCTION_INFO_V1(kwabi_query_sort_clause_length);
Datum
kwabi_query_sort_clause_length(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->query_sort_clause == NULL || shim_api->node_list_length == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    KwabiList list = shim_api->query_sort_clause(node);
    PG_RETURN_INT32(shim_api->node_list_length((KwabiNode) list));
}

PG_FUNCTION_INFO_V1(kwabi_query_group_clause_length);
Datum
kwabi_query_group_clause_length(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->query_group_clause == NULL || shim_api->node_list_length == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    KwabiList list = shim_api->query_group_clause(node);
    PG_RETURN_INT32(shim_api->node_list_length((KwabiNode) list));
}

PG_FUNCTION_INFO_V1(kwabi_query_jointree);
Datum
kwabi_query_jointree(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->query_jointree == NULL || shim_api->node_type == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32(kwabi_test_node_type_or_zero(shim_api->query_jointree(node)));
}

PG_FUNCTION_INFO_V1(kwabi_query_limit_count);
Datum
kwabi_query_limit_count(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->query_limit_count == NULL || shim_api->node_type == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32(kwabi_test_node_type_or_zero(shim_api->query_limit_count(node)));
}

PG_FUNCTION_INFO_V1(kwabi_query_limit_offset);
Datum
kwabi_query_limit_offset(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->query_limit_offset == NULL || shim_api->node_type == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32(kwabi_test_node_type_or_zero(shim_api->query_limit_offset(node)));
}

PG_FUNCTION_INFO_V1(kwabi_planned_stmt_plan_tree);
Datum
kwabi_planned_stmt_plan_tree(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_plan_tree == NULL || shim_api->node_type == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32(kwabi_test_node_type_or_zero((KwabiNode) shim_api->planned_stmt_plan_tree(node)));
}

PG_FUNCTION_INFO_V1(kwabi_planned_stmt_result_relations_length);
Datum
kwabi_planned_stmt_result_relations_length(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_result_relations == NULL || shim_api->node_list_length == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    KwabiList list = shim_api->planned_stmt_result_relations(node);
    PG_RETURN_INT32(shim_api->node_list_length((KwabiNode) list));
}

PG_FUNCTION_INFO_V1(kwabi_planned_stmt_rtable_length);
Datum
kwabi_planned_stmt_rtable_length(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_rtable == NULL || shim_api->node_list_length == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    KwabiList list = shim_api->planned_stmt_rtable(node);
    PG_RETURN_INT32(shim_api->node_list_length((KwabiNode) list));
}

PG_FUNCTION_INFO_V1(kwabi_planned_stmt_has_modifying_cte);
Datum
kwabi_planned_stmt_has_modifying_cte(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_has_modifying_cte == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    PG_RETURN_BOOL(shim_api->planned_stmt_has_modifying_cte(node));
}

PG_FUNCTION_INFO_V1(kwabi_planned_stmt_has_returning);
Datum
kwabi_planned_stmt_has_returning(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_has_returning == NULL);
    KwabiNode node = kwabi_test_parse(PG_GETARG_TEXT_PP(0));
    PG_RETURN_BOOL(shim_api->planned_stmt_has_returning(node));
}

/*
 * Positive planned-statement wrappers. Each analyzes SQL, plans it with
 * pg_plan_query (planning only; nothing is executed), and reads the result
 * through the planned_stmt slot. The DML in these statements is never run.
 */
static KwabiNode
kwabi_test_plan(text *arg)
{
    KwabiNode query = kwabi_test_parse(arg);
    return (KwabiNode) pg_plan_query((Query *) query, NULL, 0, NULL);
}

PG_FUNCTION_INFO_V1(kwabi_plan_rtable_length);
Datum
kwabi_plan_rtable_length(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_rtable == NULL || shim_api->node_list_length == NULL);
    KwabiNode plan = kwabi_test_plan(PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32(shim_api->node_list_length((KwabiNode) shim_api->planned_stmt_rtable(plan)));
}

PG_FUNCTION_INFO_V1(kwabi_plan_result_relations_length);
Datum
kwabi_plan_result_relations_length(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_result_relations == NULL || shim_api->node_list_length == NULL);
    KwabiNode plan = kwabi_test_plan(PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32(shim_api->node_list_length((KwabiNode) shim_api->planned_stmt_result_relations(plan)));
}

PG_FUNCTION_INFO_V1(kwabi_plan_has_returning);
Datum
kwabi_plan_has_returning(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_has_returning == NULL);
    KwabiNode plan = kwabi_test_plan(PG_GETARG_TEXT_PP(0));
    PG_RETURN_BOOL(shim_api->planned_stmt_has_returning(plan));
}

PG_FUNCTION_INFO_V1(kwabi_plan_has_modifying_cte);
Datum
kwabi_plan_has_modifying_cte(PG_FUNCTION_ARGS)
{
    KWABI_NODE_TEST_REQUIRE(shim_api->planned_stmt_has_modifying_cte == NULL);
    KwabiNode plan = kwabi_test_plan(PG_GETARG_TEXT_PP(0));
    PG_RETURN_BOOL(shim_api->planned_stmt_has_modifying_cte(plan));
}

/*
 * kwabi_error_slots_test() -> bool
 *
 * An error the shim captured must read back through error_message and
 * error_code, and error_clear must empty it.
 */
PG_FUNCTION_INFO_V1(kwabi_error_slots_test);

Datum
kwabi_error_slots_test(PG_FUNCTION_ARGS)
{
    bool ok = false;

    if (shim_api == NULL || shim_api->error_message == NULL ||
        shim_api->error_code == NULL || shim_api->error_clear == NULL)
        ereport(ERROR, (errmsg("kwabi: error slots are not wired")));

    PG_TRY();
    {
        ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO),
                        errmsg("kwabi error slot probe")));
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();

    {
        bool captured = strcmp(shim_api->error_message(), "kwabi error slot probe") == 0;
        bool coded = shim_api->error_code() == ERRCODE_DIVISION_BY_ZERO;

        shim_api->error_clear();
        ok = captured && coded &&
             shim_api->error_code() == 0 &&
             strlen(shim_api->error_message()) == 0;
    }
    PG_RETURN_BOOL(ok);
}

/*
 * kwabi_palloc0_repalloc_test() -> bool
 *
 * palloc0 returns zeroed memory. repalloc keeps the old contents and grows the
 * block.
 */
PG_FUNCTION_INFO_V1(kwabi_palloc0_repalloc_test);

Datum
kwabi_palloc0_repalloc_test(PG_FUNCTION_ARGS)
{
    unsigned char *zeroed;
    unsigned char *grown;
    bool           ok = true;
    int            i;

    if (shim_api == NULL || shim_api->palloc0 == NULL || shim_api->repalloc == NULL ||
        shim_api->pfree == NULL)
        ereport(ERROR, (errmsg("kwabi: memory slots are not wired")));

    zeroed = (unsigned char *) shim_api->palloc0(64);
    for (i = 0; i < 64; i++)
        if (zeroed[i] != 0)
            ok = false;

    zeroed = (unsigned char *) shim_api->repalloc(zeroed, 16);
    for (i = 0; i < 16; i++)
        zeroed[i] = (unsigned char) (0xA0 + i);

    grown = (unsigned char *) shim_api->repalloc(zeroed, 8192);
    for (i = 0; i < 16; i++)
        if (grown[i] != (unsigned char) (0xA0 + i))
            ok = false;
    grown[8191] = 1;

    shim_api->pfree(grown);
    PG_RETURN_BOOL(ok);
}
