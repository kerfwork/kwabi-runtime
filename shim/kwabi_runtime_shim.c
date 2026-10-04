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

#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "utils/builtins.h"
#include "executor/spi.h"
#include "utils/guc.h"        /* GetConfigOptionByName, SetConfigOption */
#include <dlfcn.h>            /* dlopen: see kwabi_load_extension */

/* The kwabi header names PostgreSQL types that postgres.h does not pull in.
 * Including the defining headers first is what a version-specific shim is
 * for: this is the only file in the project that knows PostgreSQL's layout. */
#include "access/htup.h"        /* HeapTuple */
#include "access/tupdesc.h"     /* TupleDesc */
#include "access/skey.h"        /* ScanKey */
#include "access/heapam.h"      /* BulkInsertState */
#include "storage/itemptr.h"    /* ItemPointer, BlockNumber, OffsetNumber */
#include "storage/buf.h"        /* Buffer, BufferAccessStrategy */
#include "storage/bufpage.h"    /* Page */
#include "storage/lwlock.h"     /* LWLock */
#include "storage/s_lock.h"     /* slock_t */
#include "nodes/pg_list.h"      /* List */
#include "nodes/nodes.h"        /* Node */
#include "nodes/primnodes.h"    /* IntoClause */
#include "nodes/plannodes.h"    /* Plan */
#include "nodes/params.h"       /* ParamListInfo */
#include "utils/relcache.h"     /* Relation */
#include "utils/reltrigger.h"   /* TriggerDesc, Trigger */
#include "utils/snapshot.h"     /* Snapshot */
#include "utils/queryenvironment.h" /* QueryEnvironment */
#include "executor/tuptable.h"  /* TupleTableSlot */
#include "executor/execdesc.h"  /* QueryDesc */
#include "commands/vacuum.h"    /* VacuumParams */
#include "lib/stringinfo.h"     /* StringInfo */
#include "utils/lsyscache.h"     /* get_element_type, get_typlen, get_typtype, getBaseType */
#include "utils/syscache.h"      /* SearchSysCache1, SysCacheGetAttr, ReleaseSysCache */
#include "catalog/pg_type.h"     /* TYPTYPE_COMPOSITE, TYPEOID */

/*
 * Version differences in this file are marked `VERSION-DIFF` so they can be
 * found with one grep. This is the complete set for 17 and 18; keeping them
 * in one file rather than copying the file per version is deliberate — a copy
 * would silently drift, and the guards below are each a fact worth reading.
 */

/* ExplainState moved out of commands/explain.h in PostgreSQL 18. */
#if PG_VERSION_NUM >= 180000
#include "commands/explain_state.h"
#else
#include "commands/explain.h"
#endif

/*
 * One name in kwabi.h has no counterpart in this PostgreSQL version, and this
 * is where that gets handled. It is the clearest small example of why the ABI
 * needs a per-version layer at all:
 *
 *   BackendId  was a real type through PG 16, then renamed to ProcNumber in
 *              PG 17 (storage/backendid.h was deleted, storage/procnumber.h
 *              added). An extension that named BackendId would fail to
 *              compile on 17+; one that uses the kwabi slot does not, because
 *              the shim absorbs the rename.
 *
 * A future ABI revision may replace the BackendId slot outright. For now the
 * shim keeps the name working, which is the whole point of the layer.
 */
/* VERSION-DIFF: BackendId -> ProcNumber at 17. */
#if PG_VERSION_NUM >= 170000
typedef ProcNumber BackendId;
#else
#include "storage/backendid.h"
#endif

/* Now kwabi.h can resolve every name in its table. Its PG type aliases would
 * collide with the real definitions we just pulled in, so they are
 * suppressed; the kwabi handle typedefs still apply. */
#define KWABI_NO_PG_TYPE_ALIASES 1
#include "kwabi.h"

/* Runtime core, from the Rust static library. The native table is defined
 * below, so this forward-declares it. */
struct KwabiNative;
extern const KwabiV1 *kwabi_runtime_init(const struct KwabiNative *native);

/*
 * Capabilities of a caller-supplied table. The runtime computes it, but it must
 * be asked about the SHIM's table: the shim installs try_body, error_get and
 * the memory accessors onto a copy of the runtime's table, so the runtime's own
 * copy has those slots NULL. Asking the wrong table understates the ABI -- it
 * reported CORE only, on a runtime that supports everything.
 */
extern uint64_t kwabi_capabilities_of(const KwabiV1 *table, uint32_t pg_major);
extern const KwabiV1 *kwabi_get_api(void);

/*
 * Overwrite the runtime's error buffer from a C-built KwabiError.
 *
 * The fmgr slots catch their errors here in C (PG_CATCH is the only place
 * CopyErrorData works), but the buffer that `error_get` reads lives in the
 * runtime. This is the bridge between them; without it the shim could catch an
 * error the extension could never read.
 */
extern void kwabi_error_set(const KwabiError *err);

/*
 * Mirror of the runtime's native table.
 *
 * The runtime owns this struct's definition. It is duplicated here because
 * the runtime is Rust and this shim is C, and neither should include the
 * other's headers. The guard below is what keeps the duplication honest: if
 * the two ever disagree about size, `kwabi_runtime_init` refuses to publish
 * and the build-time assertion fires instead of the two sides reading each
 * other's memory wrong.
 *
 * This is the one piece of hand-maintained ABI in the project, and it is
 * deliberately small — 11 fields, growing only as capability groups are
 * wired. Contrast with KwabiV1's 200 slots, which are generated.
 */
typedef struct KwabiNative {
    uint32_t pg_major;
    void *(*palloc)(size_t);
    void *(*palloc0)(size_t);
    void *(*repalloc)(void *, size_t);
    void (*pfree)(void *);
    void *(*memory_context_current)(void);
    void *(*memory_context_switch_to)(void *);
    void (*memory_context_reset)(void *);
    void (*memory_context_delete)(void *);
    const char *(*error_message)(void);
    int (*error_code)(void);
    void (*error_clear)(void);
    void (*log_line)(int, const char *);
} KwabiNative;

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
static KwabiMemoryContext
shim_memory_chunk_context(void *pointer)
{
    if (pointer == NULL)
        return NULL;
    return (KwabiMemoryContext) GetMemoryChunkContext(pointer);
}

static KwabiMemoryContext
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
static void
shim_raise_error(int sqlerrcode, const char *msg)
{
    ereport(ERROR,
            (errcode(sqlerrcode),
             errmsg("%s", msg != NULL ? msg : "kwabi: error")));
}

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
static KwabiMemoryContext
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

/* ---- shim-provided fmgr slots ---------------------------------------- */

/*
 * Capture the error currently being handled into the runtime's error buffer.
 *
 * MUST be called from inside a PG_CATCH block: CopyErrorData() reads the error
 * stack, which only exists there. On return the error state has been flushed,
 * so the caller may continue normally — which is the whole point of the
 * firewall.
 *
 * The fields are copied into KwabiError's fixed-size arrays immediately, so the
 * palloc'd ErrorData's own memory context — which a rollback is about to tear
 * down — is never depended on. That is what makes this safe to call before
 * RollbackAndReleaseCurrentSubTransaction, and it is why the "switch context
 * first" rule (error-firewall-design.md §3.2) is satisfied by the caller
 * switching to a context that outlives the subtransaction.
 */
static void
shim_capture_error(void)
{
    KwabiError err;
    ErrorData  *edata;

    kwabi_error_init(&err);

    edata = CopyErrorData();
    FlushErrorState();

    kwabi_error_set_core(&err, edata->sqlerrcode, KWABI_ERR_RAISED,
                         edata->message != NULL ? edata->message
                         : "kwabi: PostgreSQL raised without a message");
    kwabi_error_set_detail(&err, edata->detail, edata->hint);
    kwabi_error_set_object(&err,
                           edata->schema_name, edata->table_name,
                           edata->column_name, edata->datatype_name,
                           edata->constraint_name);

    FreeErrorData(edata);

    /* The runtime owns the buffer error_get reads; hand it over. */
    kwabi_error_set(&err);
}

/*
 * fmgr_info through the ABI.
 *
 * Shim-owned because fmgr_info can ereport — fmgr_info_cxt_security does
 * elog(ERROR, "cache lookup failed for function %u") — and a Rust frame between
 * that raise and a PG_TRY would break the firewall. See kwabi.h.
 *
 * NO subtransaction here, unlike the call slots below, and the distinction is
 * principled rather than an omission: fmgr_info does no user-visible work. It
 * reads catalogs and fills a struct; if it fails there is nothing to undo, so
 * the error is captured and NULL is returned. The subtransaction rule exists to
 * stop a SWALLOWED error from leaving partial WORK committed, and there is no
 * work here to leave.
 */
static KwabiFmgrInfo
shim_fmgr_info(Oid fn_oid)
{
    FmgrInfo      *flinfo = (FmgrInfo *) palloc(sizeof(FmgrInfo));
    KwabiFmgrInfo  result = NULL;

    PG_TRY();
    {
        fmgr_info(fn_oid, flinfo);
        result = (KwabiFmgrInfo) flinfo;
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

/*
 * The shared body of every call_function* slot.
 *
 * The subtransaction is MANDATORY, for the same reason try_body needs one and
 * measured in error-firewall-design.md §3.1: this slot catches a PostgreSQL
 * ERROR and RETURNS to the caller (the extension), which then continues.
 * Without a subtransaction, any partial work the failed function did before
 * erroring survives and can commit — the silent-inconsistency bug the firewall
 * exists to prevent. So the call runs in an internal subtransaction: success
 * commits it, failure rolls it back.
 *
 * Cost: a savepoint per call. That is the price of catching safely, and it is
 * the same price try_body pays. A cheaper read-only fast path is an open
 * question (error-firewall-design.md §8.3) and is deliberately not taken here,
 * because "is this call safe to run without undo?" is not a question the ABI
 * can answer for an arbitrary function.
 *
 * FunctionCallInvoke does NOT short-circuit a strict function, so a NULL
 * argument is passed through to the function; strictness is a caller-side
 * concern. See the contract in kwabi.h.
 */
static KwabiStatus
shim_call_impl(FmgrInfo *flinfo, int nargs, const Datum *args,
               const bool *argnulls, bool *isnull, Datum *result)
{
    KwabiStatus    status;
    MemoryContext  oldcontext = CurrentMemoryContext;
    ResourceOwner  oldowner   = CurrentResourceOwner;

    BeginInternalSubTransaction(NULL);

    PG_TRY();
    {
        LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
        int i;

        InitFunctionCallInfoData(*fcinfo, flinfo, nargs, InvalidOid, NULL, NULL);
        for (i = 0; i < nargs; i++)
        {
            fcinfo->args[i].value  = args[i];
            fcinfo->args[i].isnull = (argnulls != NULL) ? argnulls[i] : false;
        }

        *result = FunctionCallInvoke(fcinfo);
        *isnull = fcinfo->isnull;

        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;
        status = KWABI_OK;
    }
    PG_CATCH();
    {
        /* Switch to a context that outlives the subtransaction BEFORE
         * CopyErrorData pallocs into it — the documented order (§3.2). */
        MemoryContextSwitchTo(oldcontext);
        shim_capture_error();

        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
shim_call_function(KwabiFmgrInfo info, int nargs, Datum *args,
                   const bool *argnulls, bool *isnull, Datum *result)
{
    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;
    if (nargs < 0 || nargs > FUNC_MAX_ARGS)
        return KWABI_ERR_BAD_ARG;
    if (nargs > 0 && args == NULL)
        return KWABI_ERR_BAD_ARG;

    return shim_call_impl((FmgrInfo *) info, nargs, args, argnulls, isnull, result);
}

static KwabiStatus
shim_call_function1(KwabiFmgrInfo info, Datum arg1, bool *isnull, Datum *result)
{
    Datum a[1];

    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;

    a[0] = arg1;
    return shim_call_impl((FmgrInfo *) info, 1, a, NULL, isnull, result);
}

static KwabiStatus
shim_call_function2(KwabiFmgrInfo info, Datum arg1, Datum arg2,
                    bool *isnull, Datum *result)
{
    Datum a[2];

    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;

    a[0] = arg1;
    a[1] = arg2;
    return shim_call_impl((FmgrInfo *) info, 2, a, NULL, isnull, result);
}

static KwabiStatus
shim_call_function3(KwabiFmgrInfo info, Datum arg1, Datum arg2, Datum arg3,
                    bool *isnull, Datum *result)
{
    Datum a[3];

    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;

    a[0] = arg1;
    a[1] = arg2;
    a[2] = arg3;
    return shim_call_impl((FmgrInfo *) info, 3, a, NULL, isnull, result);
}

/* ---- shim-provided SPI slots ---------------------------------------- */

/*
 * The SPI result handle.
 *
 * SPI is a per-backend global state: SPI_tuptable and SPI_processed are
 * globals that are overwritten by the next SPI command. So the result
 * handle copies them out immediately, and the caller reads from the copy.
 *
 * The tuple table itself is NOT copied: it is owned by SPI and is only
 * valid until the next SPI command or until SPI_finish is called. So
 * spi_free_result must be called before the next spi_execute, and the
 * caller must read all values before then. This is a limitation, but it
 * is consistent with how SPI works in PostgreSQL.
 */
typedef struct KwabiSPIResultImpl {
    SPITupleTable *tuptable;
    uint64 processed;
} KwabiSPIResultImpl;

static KwabiSPIResult
shim_spi_execute(const char *sql, bool read_only, int tcount)
{
    KwabiSPIResultImpl *result;
    int spi_result;

    if (sql == NULL)
        return NULL;

    if (SPI_connect() != SPI_OK_CONNECT)
        return NULL;

    spi_result = SPI_execute(sql, read_only, tcount);
    if (spi_result < 0) {
        SPI_finish();
        return NULL;
    }

    result = (KwabiSPIResultImpl *) palloc(sizeof(KwabiSPIResultImpl));
    result->tuptable = SPI_tuptable;
    result->processed = SPI_processed;

    return (KwabiSPIResult) result;
}

static KwabiSPIResult
shim_spi_execute_plan(KwabiSPIPlan plan, Datum *values, const char *nulls,
                      bool read_only, int tcount)
{
    KwabiSPIResultImpl *result;
    int spi_result;

    if (plan == NULL)
        return NULL;

    if (SPI_connect() != SPI_OK_CONNECT)
        return NULL;

    spi_result = SPI_execute_plan((SPIPlanPtr) plan, values, nulls, read_only, tcount);
    if (spi_result < 0) {
        SPI_finish();
        return NULL;
    }

    result = (KwabiSPIResultImpl *) palloc(sizeof(KwabiSPIResultImpl));
    result->tuptable = SPI_tuptable;
    result->processed = SPI_processed;

    return (KwabiSPIResult) result;
}

static void
shim_spi_free_result(KwabiSPIResult result)
{
    if (result == NULL)
        return;

    SPI_finish();
    pfree(result);
}

static int
shim_spi_result_ntuples(KwabiSPIResult result)
{
    KwabiSPIResultImpl *impl = (KwabiSPIResultImpl *) result;
    if (impl == NULL)
        return 0;
    return (int) impl->processed;
}

static Datum
shim_spi_result_get_value(KwabiSPIResult result, int tupno, int attno)
{
    KwabiSPIResultImpl *impl = (KwabiSPIResultImpl *) result;
    bool isnull;

    if (impl == NULL || impl->tuptable == NULL)
        return (Datum) 0;
    if (tupno < 0 || tupno >= (int) impl->processed)
        return (Datum) 0;
    if (attno < 1 || attno > impl->tuptable->tupdesc->natts)
        return (Datum) 0;

    return SPI_getbinval(impl->tuptable->vals[tupno],
                         impl->tuptable->tupdesc, attno, &isnull);
}

/* ---- shim-provided GUC slots ---------------------------------------- */

/*
 * GUC access through the ABI.
 *
 * These are shim-owned because GetConfigOptionByName and SetConfigOption
 * can raise (e.g. "unrecognized configuration parameter"), and the error
 * firewall forbids a Rust frame between a PG_TRY and a raising call.
 *
 * The signatures match kwabi.h exactly:
 *   int (*guc_get_int)(const char *name);
 *   const char *(*guc_get_string)(const char *name);
 *   bool (*guc_get_bool)(const char *name);
 *   double (*guc_get_float)(const char *name);
 *   void (*guc_set_int)(const char *name, int value);
 *   void (*guc_set_string)(const char *name, const char *value);
 *   void (*guc_set_bool)(const char *name, bool value);
 *   void (*guc_set_float)(const char *name, double value);
 */

static int
shim_guc_get_int(const char *name)
{
    char        *str;
    int          value = 0;

    if (name == NULL)
        return 0;

    PG_TRY();
    {
        str = GetConfigOptionByName(name, NULL, false);
        if (str != NULL)
            value = atoi(str);
    }
    PG_CATCH();
    {
        shim_capture_error();
        value = 0;
    }
    PG_END_TRY();

    return value;
}

static const char *
shim_guc_get_string(const char *name)
{
    char        *str;

    if (name == NULL)
        return NULL;

    PG_TRY();
    {
        str = GetConfigOptionByName(name, NULL, false);
        return str;
    }
    PG_CATCH();
    {
        shim_capture_error();
        return NULL;
    }
    PG_END_TRY();
}

static bool
shim_guc_get_bool(const char *name)
{
    char        *str;
    bool         value = false;

    if (name == NULL)
        return false;

    PG_TRY();
    {
        str = GetConfigOptionByName(name, NULL, false);
        if (str != NULL)
            value = (str[0] == 't' || str[0] == 'T' || str[0] == '1' || str[0] == 'o');
    }
    PG_CATCH();
    {
        shim_capture_error();
        value = false;
    }
    PG_END_TRY();

    return value;
}

static double
shim_guc_get_float(const char *name)
{
    char        *str;
    double       value = 0.0;

    if (name == NULL)
        return 0.0;

    PG_TRY();
    {
        str = GetConfigOptionByName(name, NULL, false);
        if (str != NULL)
            value = atof(str);
    }
    PG_CATCH();
    {
        shim_capture_error();
        value = 0.0;
    }
    PG_END_TRY();

    return value;
}

static void
shim_guc_set_int(const char *name, int value)
{
    char         buf[64];

    if (name == NULL)
        return;

    snprintf(buf, sizeof(buf), "%d", value);

    PG_TRY();
    {
        SetConfigOption(name, buf, PGC_USERSET, PGC_S_SESSION);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_guc_set_string(const char *name, const char *value)
{
    if (name == NULL)
        return;

    PG_TRY();
    {
        SetConfigOption(name, value != NULL ? value : "", PGC_USERSET, PGC_S_SESSION);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_guc_set_bool(const char *name, bool value)
{
    if (name == NULL)
        return;

    PG_TRY();
    {
        SetConfigOption(name, value ? "on" : "off", PGC_USERSET, PGC_S_SESSION);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_guc_set_float(const char *name, double value)
{
    char         buf[64];

    if (name == NULL)
        return;

    snprintf(buf, sizeof(buf), "%g", value);

    PG_TRY();
    {
        SetConfigOption(name, buf, PGC_USERSET, PGC_S_SESSION);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

/* ---- shim-provided defrem slots -------------------------------------- */

/*
 * Default (defrem) operations through the ABI.
 *
 * These use SPI to execute ALTER TABLE statements, which is the safest
 * approach: it goes through PostgreSQL's own parser and executor, so
 * the semantics are exactly PostgreSQL's.
 *
 * The signatures match kwabi.h:
 *   void (*defrem_create)(const char *name, const char *type, const char *value);
 *   void (*defrem_alter)(const char *name, const char *value);
 *   void (*defrem_drop)(const char *name);
 *
 * `name` is expected in "table.column" format.
 */

/*
 * Split "table.column" into table and column parts.
 * Returns true if the split succeeded, false if there was no dot.
 */
static bool
split_table_column(const char *name, char *table, size_t table_len,
                   char *column, size_t column_len)
{
    const char *dot;

    if (name == NULL || table == NULL || column == NULL)
        return false;

    dot = strrchr(name, '.');
    if (dot == NULL)
        return false;

    if ((size_t)(dot - name) >= table_len)
        return false;
    if (strlen(dot + 1) >= column_len)
        return false;

    snprintf(table, table_len, "%.*s", (int)(dot - name), name);
    snprintf(column, column_len, "%s", dot + 1);
    return true;
}

static void
shim_defrem_create(const char *name, const char *type, const char *value)
{
    char         sql[1024];
    char         table[256];
    char         column[256];

    if (name == NULL || type == NULL || value == NULL)
        return;

    if (!split_table_column(name, table, sizeof(table), column, sizeof(column)))
        return;

    snprintf(sql, sizeof(sql),
             "ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
             table, column, value);

    if (SPI_connect() != SPI_OK_CONNECT)
        return;

    if (SPI_execute(sql, false, 0) < 0) {
        SPI_finish();
        return;
    }

    SPI_finish();
}

static void
shim_defrem_alter(const char *name, const char *value)
{
    char         sql[1024];
    char         table[256];
    char         column[256];

    if (name == NULL || value == NULL)
        return;

    if (!split_table_column(name, table, sizeof(table), column, sizeof(column)))
        return;

    snprintf(sql, sizeof(sql),
             "ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
             table, column, value);

    if (SPI_connect() != SPI_OK_CONNECT)
        return;

    if (SPI_execute(sql, false, 0) < 0) {
        SPI_finish();
        return;
    }

    SPI_finish();
}

static void
shim_defrem_drop(const char *name)
{
    char         sql[1024];
    char         table[256];
    char         column[256];

    if (name == NULL)
        return;

    if (!split_table_column(name, table, sizeof(table), column, sizeof(column)))
        return;

    snprintf(sql, sizeof(sql),
             "ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT",
             table, column);

    if (SPI_connect() != SPI_OK_CONNECT)
        return;

    if (SPI_execute(sql, false, 0) < 0) {
        SPI_finish();
        return;
    }

    SPI_finish();
}

/* ---- shim-provided type system slots --------------------------------- */

/*
 * Type system access through the ABI.
 *
 * These are shim-owned because catalog lookups can raise (e.g. "type does
 * not exist"), and the error firewall forbids a Rust frame between a PG_TRY
 * and a raising call.
 *
 * The signatures match kwabi.h exactly:
 *   Datum (*type_input)(Oid type_oid, const char *input, int32 typmod);
 *   char *(*type_output)(Oid type_oid, Datum value);
 *   Datum (*type_recv)(Oid type_oid, StringInfo buf);
 *   void (*type_send)(Oid type_oid, Datum value, StringInfo buf);
 *   Oid (*type_element_type)(Oid type_oid);
 *   int16 (*type_length)(Oid type_oid);
 *   bool (*type_is_array)(Oid type_oid);
 *   bool (*type_is_composite)(Oid type_oid);
 *   Oid (*type_base_type)(Oid type_oid);
 */

/* Look up a type's I/O function OID from the catalog cache. */
static Oid
get_type_io_func(Oid type_oid, int which)
{
    HeapTuple tup;
    Oid func_oid = InvalidOid;
    bool isnull;

    if (!OidIsValid(type_oid))
        return InvalidOid;

    tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
    if (!HeapTupleIsValid(tup))
        return InvalidOid;

    switch (which) {
        case 0: /* input */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typinput, &isnull));
            break;
        case 1: /* output */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typoutput, &isnull));
            break;
        case 2: /* receive */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typreceive, &isnull));
            break;
        case 3: /* send */
            func_oid = DatumGetObjectId(SysCacheGetAttr(TYPEOID, tup, Anum_pg_type_typsend, &isnull));
            break;
    }

    ReleaseSysCache(tup);
    return func_oid;
}

static Datum
shim_type_input(Oid type_oid, const char *input, int32 typmod)
{
    Oid input_func;
    Datum result = (Datum) 0;

    if (!OidIsValid(type_oid) || input == NULL)
        return (Datum) 0;

    input_func = get_type_io_func(type_oid, 0);
    if (!OidIsValid(input_func))
        return (Datum) 0;

    PG_TRY();
    {
        result = OidInputFunctionCall(input_func, (char *) input, type_oid, typmod);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = (Datum) 0;
    }
    PG_END_TRY();

    return result;
}

static char *
shim_type_output(Oid type_oid, Datum value)
{
    Oid output_func;
    char *result = NULL;

    if (!OidIsValid(type_oid))
        return NULL;

    output_func = get_type_io_func(type_oid, 1);
    if (!OidIsValid(output_func))
        return NULL;

    PG_TRY();
    {
        result = OidOutputFunctionCall(output_func, value);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

static Datum
shim_type_recv(Oid type_oid, StringInfo buf)
{
    Oid recv_func;
    Datum result = (Datum) 0;

    if (!OidIsValid(type_oid) || buf == NULL)
        return (Datum) 0;

    recv_func = get_type_io_func(type_oid, 2);
    if (!OidIsValid(recv_func))
        return (Datum) 0;

    PG_TRY();
    {
        result = OidReceiveFunctionCall(recv_func, buf, type_oid, -1);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = (Datum) 0;
    }
    PG_END_TRY();

    return result;
}

static void
shim_type_send(Oid type_oid, Datum value, StringInfo buf)
{
    Oid send_func;
    bytea *result;

    if (!OidIsValid(type_oid) || buf == NULL)
        return;

    send_func = get_type_io_func(type_oid, 3);
    if (!OidIsValid(send_func))
        return;

    PG_TRY();
    {
        result = OidSendFunctionCall(send_func, value);
        if (result != NULL) {
            appendBinaryStringInfo(buf, VARDATA(result), VARSIZE(result) - VARHDRSZ);
        }
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static Oid
shim_type_element_type(Oid type_oid)
{
    Oid result = InvalidOid;

    if (!OidIsValid(type_oid))
        return InvalidOid;

    PG_TRY();
    {
        result = get_element_type(type_oid);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidOid;
    }
    PG_END_TRY();

    return result;
}

static int16
shim_type_length(Oid type_oid)
{
    int16 result = 0;

    if (!OidIsValid(type_oid))
        return 0;

    PG_TRY();
    {
        result = get_typlen(type_oid);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = 0;
    }
    PG_END_TRY();

    return result;
}

static bool
shim_type_is_array(Oid type_oid)
{
    bool result = false;

    if (!OidIsValid(type_oid))
        return false;

    PG_TRY();
    {
        result = OidIsValid(get_element_type(type_oid));
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = false;
    }
    PG_END_TRY();

    return result;
}

static bool
shim_type_is_composite(Oid type_oid)
{
    bool result = false;
    char typtype;

    if (!OidIsValid(type_oid))
        return false;

    PG_TRY();
    {
        typtype = get_typtype(type_oid);
        result = (typtype == TYPTYPE_COMPOSITE);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = false;
    }
    PG_END_TRY();

    return result;
}

static Oid
shim_type_base_type(Oid type_oid)
{
    Oid result = InvalidOid;

    if (!OidIsValid(type_oid))
        return InvalidOid;

    PG_TRY();
    {
        result = getBaseType(type_oid);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidOid;
    }
    PG_END_TRY();

    return result;
}

static const char *
native_error_message(void)
{
    return "postgres: see ErrorData";
}

static int
native_error_code(void)
{
    return geterrcode();
}

static void
native_error_clear(void)
{
    /* PostgreSQL clears its error state at transaction boundaries; there is
     * nothing useful to do here without an active ErrorData. */
}

static void
native_log_line(int level, const char *msg)
{
    ereport(level,
            (errmsg("%s", msg != NULL ? msg : "")));
}

static void
install_native(KwabiNative *n)
{
    /*
     * PG_VERSION_NUM is 180006 on 18.6 -- major, minor, patch packed as
     * Mmmppp. The major is the M part, so the divisor is 10000, not 100.
     *
     * This was wrong (/100, giving 1800) from the start and went unnoticed
     * because nothing read the field. The capability bitset is the first
     * consumer, and it exposed the bug immediately: ATOMIC_BODY is granted
     * only for majors the CI matrix has measured, so an unmatchable 1800
     * silently withheld a bit the runtime genuinely supports.
     *
     * Worth remembering as a pattern: a field with no reader is untested, and
     * its correctness is discovered by its first consumer.
     */
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
static KwabiV1 shim_table;
static const KwabiV1 *shim_api = NULL;

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

    /*
     * The fmgr group, shim-owned for the same reason as raise_error: these call
     * into PostgreSQL, which can longjmp. Putting them in the runtime would put
     * a Rust frame between the raise and this file's PG_TRY.
     */
    shim_table.fmgr_info = shim_fmgr_info;
    shim_table.call_function = shim_call_function;
    shim_table.call_function1 = shim_call_function1;
    shim_table.call_function2 = shim_call_function2;
    shim_table.call_function3 = shim_call_function3;

    /*
     * The SPI group, shim-owned for the same reason as the fmgr group: SPI
     * calls can raise, and the error firewall forbids a Rust frame between
     * a PG_TRY and a raising call. SPI_connect/SPI_execute/SPI_finish are
     * ordinary PostgreSQL symbols the shim reaches directly.
     */
    shim_table.spi_execute = shim_spi_execute;
    shim_table.spi_execute_plan = shim_spi_execute_plan;
    shim_table.spi_free_result = shim_spi_free_result;
    shim_table.spi_result_ntuples = shim_spi_result_ntuples;
    shim_table.spi_result_get_value = shim_spi_result_get_value;

    /*
     * The GUC group, shim-owned for the same reason as the fmgr group: GUC
     * calls can raise, and the error firewall forbids a Rust frame between
     * a PG_TRY and a raising call. GetConfigOptionByName and SetConfigOption
     * are ordinary PostgreSQL symbols the shim reaches directly.
     */
    shim_table.guc_get_int = shim_guc_get_int;
    shim_table.guc_get_string = shim_guc_get_string;
    shim_table.guc_get_bool = shim_guc_get_bool;
    shim_table.guc_get_float = shim_guc_get_float;
    shim_table.guc_set_int = shim_guc_set_int;
    shim_table.guc_set_string = shim_guc_set_string;
    shim_table.guc_set_bool = shim_guc_set_bool;
    shim_table.guc_set_float = shim_guc_set_float;

    /*
     * The defrem group, shim-owned for the same reason: ALTER TABLE can
     * raise, and the error firewall forbids a Rust frame between a PG_TRY
     * and a raising call. SPI_connect/SPI_execute/SPI_finish are ordinary
     * PostgreSQL symbols the shim reaches directly.
     */
    shim_table.defrem_create = shim_defrem_create;
    shim_table.defrem_alter = shim_defrem_alter;
    shim_table.defrem_drop = shim_defrem_drop;

    /*
     * The type system group, shim-owned for the same reason: catalog lookups
     * can raise, and the error firewall forbids a Rust frame between a PG_TRY
     * and a raising call.
     */
    shim_table.type_input = shim_type_input;
    shim_table.type_output = shim_type_output;
    shim_table.type_recv = shim_type_recv;
    shim_table.type_send = shim_type_send;
    shim_table.type_element_type = shim_type_element_type;
    shim_table.type_length = shim_type_length;
    shim_table.type_is_array = shim_type_is_array;
    shim_table.type_is_composite = shim_type_is_composite;
    shim_table.type_base_type = shim_type_base_type;

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
static uint64_t
shim_capabilities(void)
{
    return kwabi_capabilities_of(&shim_table, (uint32_t) (PG_VERSION_NUM / 10000));
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
    StringInfoData buf;

    initStringInfo(&buf);

    if (ntuples > 0) {
        /* Read the first row's first column to prove get_value works. */
        Datum d = api->spi_result_get_value(result, 0, 1);
        appendStringInfo(&buf, "rows=%d first_datum=%lu", ntuples,
                         (unsigned long) d);
    } else {
        appendStringInfo(&buf, "rows=0");
    }

    api->spi_free_result(result);
    pfree(sql);

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

    /* Select it back. */
    KwabiSPIResult r3 = api->spi_execute(
        "SELECT val FROM kwabi_spi_t", true, 0);
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
    int32 actual = shim_api->guc_get_int("work_mem");

    PG_RETURN_BOOL(actual == expected);
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

    char *expected = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *actual = shim_api->guc_get_string("server_version");

    bool result = (actual != NULL && strcmp(actual, expected) == 0);
    pfree(expected);

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

    float8 expected = PG_GETARG_FLOAT8(0);
    double actual = shim_api->guc_get_float("shared_buffers");

    PG_RETURN_BOOL(fabs(actual - expected) < 0.001);
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
                     true, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    bool default_set = false;
    if (SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            char *expr = DatumGetCString(d);
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
                     true, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: SELECT failed")));
    }

    bool default_is_99 = false;
    if (SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            char *expr = DatumGetCString(d);
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
