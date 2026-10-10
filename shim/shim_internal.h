#ifndef SHIM_INTERNAL_H
#define SHIM_INTERNAL_H

/*
 * shim_internal.h — common includes and declarations for the kwabi shim.
 *
 * Every shim .c file includes this header. It pulls in all PostgreSQL
 * headers, the kwabi ABI header, and declares the shared types and
 * functions that the group files and the main file need.
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
#include "access/htup_details.h" /* HeapTupleHeaderGetOid */
#include "access/tupdesc.h"     /* TupleDesc */
#include "access/skey.h"        /* ScanKey */
#include "access/heapam.h"      /* BulkInsertState */
#include "access/xact.h"        /* StartTransaction, CommitTransaction, AbortCurrentTransaction */
#include "storage/itemptr.h"    /* ItemPointer, BlockNumber, OffsetNumber */
#include "storage/buf.h"        /* Buffer, BufferAccessStrategy */
#include "storage/bufmgr.h"     /* ReadBuffer, ReleaseBuffer, MarkBufferDirty */
#include "storage/bufpage.h"    /* Page */
#include "storage/lwlock.h"     /* LWLock */
#include "storage/s_lock.h"     /* slock_t */
#include "storage/shmem.h"      /* ShmemAlloc, ShmemInitStruct, RequestAddinShmemSpace */
#include "storage/ipc.h"        /* shmem_request_hook, shmem_startup_hook */
#include "miscadmin.h"          /* process_shared_preload_libraries_in_progress */
#include "storage/fd.h"         /* MakePGDirectory */
#include "access/slru.h"        /* SlruCtl, SimpleLruInit, SimpleLruShmemSize */
#include "nodes/pg_list.h"      /* List */
#include "nodes/nodes.h"        /* Node */
#include "nodes/primnodes.h"    /* IntoClause */
#include "nodes/plannodes.h"    /* Plan */
#include "parser/parser.h"     /* pg_parse_query, pg_plan_query */
#include "nodes/params.h"       /* ParamListInfo */
#include "utils/relcache.h"     /* Relation */
#include "utils/reltrigger.h"   /* TriggerDesc, Trigger */
#include "utils/snapshot.h"     /* Snapshot */
/* QueryEnvironment is not included — the ABI treats it as void * */
#include "executor/tuptable.h"  /* TupleTableSlot */
#include "executor/execdesc.h"  /* QueryDesc */
#include "commands/vacuum.h"    /* VacuumParams */
#include "commands/extension.h" /* get_extension_oid, get_extension_version */
#include "lib/stringinfo.h"     /* StringInfo */
#include "utils/lsyscache.h"     /* get_element_type, get_typlen, get_typtype, getBaseType */
#include "utils/syscache.h"      /* SearchSysCache1, SysCacheGetAttr, ReleaseSysCache */
#include "optimizer/planner.h"   /* standard_planner */
#include "optimizer/cost.h"       /* cost_qual_eval */
#include "catalog/pg_type.h"     /* TYPTYPE_COMPOSITE, TYPEOID */
#include "catalog/pg_operator.h" /* OPEROID, Anum_pg_operator_oprleft, etc. */
#include "catalog/pg_extension.h" /* Anum_pg_extension_extversion */
#include "parser/parse_type.h"   /* parseTypeString, typeStringToTypeName */
#include "parser/analyze.h"       /* parse_analyze_fixedparams */
#include "parser/parser.h"       /* pg_parse_query, pg_plan_query */

/* ExplainState moved out of commands/explain.h in PostgreSQL 18.
 * Include both: explain.h has ExplainOnePlan, explain_state.h has ExplainState. */
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_state.h"
#endif
#include "utils/plancache.h" /* pg_plan_query */

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
#define LWLock void *
#define slock_t void *
#define QueryEnvironment void *
#include "kwabi.h"
#undef LWLock
#undef slock_t
/* QueryEnvironment stays void* throughout the shim — the ABI type */

/* ── Rust runtime core ─────────────────────────────────────────────────── */

struct KwabiNative;
extern const KwabiV1 *kwabi_runtime_init(const struct KwabiNative *native);
extern uint64_t kwabi_capabilities_of(const KwabiV1 *table, uint32_t pg_major);
extern const KwabiV1 *kwabi_get_api(void);
extern void kwabi_error_set(const KwabiError *err);
extern int kwabi_try_body(KwabiBodyFn body, void *arg, KwabiError *out);
extern void kwabi_error_get(KwabiError *out);

/* ── KwabiNative: mirror of the runtime's native table ─────────────────── */

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

/* ── Shared shim state (defined in main file) ──────────────────────────── */

extern KwabiV1 shim_table;
extern const KwabiV1 *shim_api;

/* ── Error capture (defined in group_fmgr.c) ───────────────────────────── */

extern void shim_capture_error(void);

/* Report a slot that cannot give a true answer, without raising: the error is left for
 * the extension to read through error_get / error_code. Defined in group_node.c. */
extern void shim_unsupported(int sqlerrcode, const char *msg);

/* ── Group init functions ──────────────────────────────────────────────── */

extern void init_group_fmgr(void);
extern void init_group_spi(void);
extern void init_group_guc(void);
extern void init_group_defrem(void);
extern void init_group_type(void);
extern void init_group_stringinfo(void);
extern void init_group_shmem(void);
extern void init_group_parser(void);
extern void init_group_tuple(void);
extern void init_group_relation(void);
extern void init_group_buffer(void);
extern void init_group_syscache(void);
extern void init_group_lwlock(void);
extern void init_group_node(void);
extern void init_group_logical(void);
extern void init_group_trigger(void);
extern void init_group_tableam(void);
extern void init_group_lock(void);
extern void init_group_extension(void);
extern void init_group_explain(void);
extern void init_group_transaction(void);
extern void init_group_executor(void);
extern void init_group_hook(void);
extern void shim_hook_install_reload(void);
extern bool shim_hook_reload_available(void);
extern bool shim_type_bodies_lookup(const char *name, KwabiTypeBodies *out);
extern void shim_raise_kwabi_error(const KwabiError *err);
extern bool shim_agg_bodies_lookup(const char *name, KwabiAggBodies *out);
extern void init_group_bgworker(void);
extern void init_group_slru(void);

/* ── SLRU group: preload hooks + the create/attach path ────────────────── */

/* Called from _PG_init. Registers the shmem request/startup hooks. Returns
 * true if the runtime is being loaded via shared_preload_libraries, false if
 * it is a plain LOAD (in which case SLRU is unavailable and the capability
 * bit must not be claimed). */
extern bool shim_slru_install_hooks(void);

/* True if the preload hooks ran and at least one SLRU was initialised at
 * startup. Read by the capability computation. */
extern bool shim_slru_is_available(void);

#endif /* SHIM_INTERNAL_H */
