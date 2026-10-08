/* group_slru.c — SLRU slots for the kwabi shim.
 *
 * # Why SLRU is not like the other groups
 *
 * Every other slot group is a straight forward: the shim calls a PostgreSQL
 * function and returns its result. SLRU cannot be, because PostgreSQL requires
 * an SLRU's shared memory to be reserved BEFORE the postmaster forks. A backend
 * that reaches `SimpleLruInit` after fork finds `IsUnderPostmaster == true`,
 * takes the attach branch, and gets an SLRU whose `num_slots` is zero — the
 * initialisation block never ran. That is a property of PostgreSQL, not a bug
 * in the shim.
 *
 * So this group has two halves:
 *
 *   1. PRELOAD (shmem_request_hook + shmem_startup_hook). The runtime must be
 *      in shared_preload_libraries. The request hook reserves space for each
 *      declared SLRU plus its control-lock state; the startup hook initialises
 *      each one in the postmaster. Backends then inherit the initialised ctl
 *      through fork.
 *
 *   2. RUNTIME (slru_create / slru_read / slru_write). Because the postmaster
 *      already did the work, `slru_create` is a validated lookup, not an
 *      initialisation. It exists so the ABI keeps its shape: an extension asks
 *      for an SLRU by name and gets a usable handle.
 *
 * # The declaration set
 *
 * The request hook runs before any extension exists, so it cannot ask an
 * extension which SLRUs it wants. The set is therefore declared up front via
 * the `kwabi.slrus` GUC (a comma-separated list of names), read in the request
 * hook. Default empty: a runtime preloaded with no declaration reserves
 * nothing and reports SLRU unavailable.
 *
 * # Version split
 *
 * `SimpleLruInit` has two signatures (see the guard below) and PG 16 has no
 * SLRU banks at all: PG 16 serialises all SLRU access on one control LWLock
 * that the caller supplies, while 17/18 use per-bank LWLocks with tranche ids.
 * The two paths are kept separate rather than unified, because they are not the
 * same mechanism.
 */

#include "shim_internal.h"

/* ── configuration ─────────────────────────────────────────────────────── */

/* How many SLRUs a preloaded runtime can declare. Fixed, because the request
 * hook must know the total shmem requirement before any name is known. */
#define KWABI_SLRU_MAX      8

/* Page slots per declared SLRU. 16 is the minimum (one bank) and is what
 * PostgreSQL's own test_slru uses. */
#define KWABI_SLRU_BUFFERS  16

/* GUC name for the declaration list. */
#define KWABI_SLRU_GUC      "kwabi.slrus"

/* ── per-SLRU state ────────────────────────────────────────────────────── */

/*
 * The control-lock state PG 16 needs. PG 16's SimpleLruInit takes an LWLock *
 * and stores it as shared->ControlLock; a third-party SLRU has no built-in
 * lock to pass, so it must allocate one in shared memory itself. PG 17/18 do
 * not use this at all (they allocate their own bank locks from tranche ids),
 * but the struct is kept for both versions so the registry has one shape.
 */
typedef struct KwabiSlruSharedState
{
    LWLock  lock;
} KwabiSlruSharedState;

/* One declared SLRU. `ctl` is the postmaster-initialised control struct; every
 * backend inherits the same pointer through fork, which is why it is a plain
 * static and not re-derived per backend. */
typedef struct KwabiSlruEntry
{
    char                 name[NAMEDATALEN];
    bool                 in_use;
    SlruCtlData          ctl;
    KwabiSlruSharedState *state;    /* PG 16 control lock; NULL on 17/18 */
} KwabiSlruEntry;

static KwabiSlruEntry     kwabi_slrus[KWABI_SLRU_MAX];
static int                kwabi_slru_count = 0;
static bool               kwabi_slru_preloaded = false;
static bool               kwabi_slru_initialised = false;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* ── helpers ───────────────────────────────────────────────────────────── */

static KwabiSlruEntry *
kwabi_slru_lookup(const char *name)
{
    if (name == NULL)
        return NULL;

    for (int i = 0; i < kwabi_slru_count; i++)
    {
        if (kwabi_slrus[i].in_use && strcmp(kwabi_slrus[i].name, name) == 0)
            return &kwabi_slrus[i];
    }
    return NULL;
}

/*
 * Default PagePrecedes for a generic SLRU.
 *
 * The core SLRUs implement modular ("wrap around") comparison because their
 * page numbers are transaction ids. A kwabi SLRU has no such mapping, so a
 * plain linear comparison is the correct choice: it never wraps, and linear
 * order is what an extension using this as a bounded journal expects.
 *
 * The callback signature differs by version: PG 16 declares PagePrecedes as
 * bool (*)(int, int), PG 17/18 as bool (*)(int64, int64). Two definitions is
 * the honest way to absorb that — a single one cannot satisfy both.
 */
#if PG_VERSION_NUM >= 170000
static bool
kwabi_slru_page_precedes(int64 page1, int64 page2)
{
    return page1 < page2;
}
#else
static bool
kwabi_slru_page_precedes(int page1, int page2)
{
    return page1 < page2;
}
#endif

/*
 * Parse the kwabi.slrus GUC into the declaration list.
 *
 * Runs in the postmaster during the request hook, before any backend exists.
 * A malformed or oversized list is truncated rather than fatal: a runtime that
 * refuses to start because of a bad GUC is worse than one that starts with
 * fewer SLRUs.
 */
static void
kwabi_slru_parse_guc(void)
{
    char  *raw;
    char  *tok;
    char  *saveptr = NULL;

    kwabi_slru_count = 0;

    /* GetConfigOptionByName is safe in the postmaster for a PGC_POSTMASTER or
     * PGC_SIGHUP GUC; a plain string GUC is fine. */
    raw = GetConfigOptionByName(KWABI_SLRU_GUC, NULL, true);
    if (raw == NULL || raw[0] == '\0')
        return;

    for (tok = strtok_r(raw, ",", &saveptr);
         tok != NULL && kwabi_slru_count < KWABI_SLRU_MAX;
         tok = strtok_r(NULL, ",", &saveptr))
    {
        /* trim leading spaces */
        while (*tok == ' ' || *tok == '\t')
            tok++;

        if (*tok == '\0')
            continue;

        KwabiSlruEntry *e = &kwabi_slrus[kwabi_slru_count];
        memset(e, 0, sizeof(*e));
        strlcpy(e->name, tok, sizeof(e->name));
        e->in_use = true;
        kwabi_slru_count++;
    }
}

/* ── preload: request ──────────────────────────────────────────────────── */

static void
kwabi_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    kwabi_slru_parse_guc();

    for (int i = 0; i < kwabi_slru_count; i++)
    {
        /* The SLRU's own buffers. */
        RequestAddinShmemSpace(SimpleLruShmemSize(KWABI_SLRU_BUFFERS, 0));
        /* PG 16 only: the control lock that SimpleLruInit will store. */
        RequestAddinShmemSpace(MAXALIGN(sizeof(KwabiSlruSharedState)));
    }
}

/* ── preload: startup ──────────────────────────────────────────────────── */

/*
 * Define the kwabi.slrus GUC.
 *
 * It must exist before the request hook reads it, and both run in the
 * postmaster, so it is defined from _PG_init when the runtime is being
 * preloaded. PGC_POSTMASTER: the declaration set sizes shared memory, so it
 * cannot change without a restart.
 */
static char *kwabi_slru_guc_value = NULL;

static void
kwabi_slru_define_guc(void)
{
    DefineCustomStringVariable(KWABI_SLRU_GUC,
                               "Comma-separated SLRU names the kwabi runtime should create.",
                               "Each name is initialised at postmaster startup and can be "
                               "attached from any backend through slru_create.",
                               &kwabi_slru_guc_value,
                               "",
                               PGC_POSTMASTER,
                               0,        /* no flags: restart-only is implied by PGC_POSTMASTER */
                               NULL, NULL, NULL);
}

static void
kwabi_shmem_startup(void)
{
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    for (int i = 0; i < kwabi_slru_count; i++)
    {
        KwabiSlruEntry *e = &kwabi_slrus[i];
        char            base[MAXPGPATH];
        char            dir[MAXPGPATH];

        /*
         * Create the directory tree, parent first.
         *
         * MakePGDirectory does NOT create parents (it is a bare mkdir), and
         * the SLRU write path only opens the FILE with O_CREAT — it does not
         * create the directory. So a missing parent here does not fail loudly:
         * it makes the first write fail with ENOENT, reported as "could not
         * access status of transaction". Both levels must be created, and
         * EEXIST must be tolerated because the postmaster may re-run this.
         */
        snprintf(base, sizeof(base), "pg_kwabi_slru");
        snprintf(dir, sizeof(dir), "pg_kwabi_slru/%s", e->name);

        if (MakePGDirectory(base) < 0 && errno != EEXIST)
            ereport(WARNING,
                    (errmsg("kwabi: could not create SLRU base directory \"%s\": %m",
                            base)));
        if (MakePGDirectory(dir) < 0 && errno != EEXIST)
            ereport(WARNING,
                    (errmsg("kwabi: could not create SLRU directory \"%s\": %m",
                            dir)));

#if PG_VERSION_NUM >= 170000
        /* PG 17/18: banks allocate their own locks from tranche ids. */
        {
            int buffer_tranche_id = LWLockNewTrancheId();
            int bank_tranche_id   = LWLockNewTrancheId();

            LWLockRegisterTranche(buffer_tranche_id, e->name);
            LWLockRegisterTranche(bank_tranche_id, e->name);

            e->ctl.PagePrecedes = kwabi_slru_page_precedes;
            SimpleLruInit(&e->ctl, e->name, KWABI_SLRU_BUFFERS, 0, dir,
                          buffer_tranche_id, bank_tranche_id,
                          SYNC_HANDLER_NONE, false);
        }
#else
        /* PG 16: one caller-supplied control lock, one tranche id.
         *
         * The lock's shmem entry needs a name of its OWN. The SLRU's
         * SimpleLruInit also registers a shmem entry under e->name, and the
         * shmem index keys on name: reusing it here fails at startup with
         * "ShmemIndex entry size is wrong for data structure ... expected
         * <big>, actual 16". Suffixing "_lock" keeps the two distinct.
         */
        {
            char lockname[NAMEDATALEN + 8];
            bool found = false;
            int  tranche_id = LWLockNewTrancheId();

            snprintf(lockname, sizeof(lockname), "%s_lock", e->name);

            LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
            e->state = ShmemInitStruct(lockname, sizeof(KwabiSlruSharedState),
                                       &found);
            if (!found)
                LWLockInitialize(&e->state->lock, tranche_id);
            LWLockRelease(AddinShmemInitLock);

            LWLockRegisterTranche(tranche_id, e->name);

            e->ctl.PagePrecedes = kwabi_slru_page_precedes;
            SimpleLruInit(&e->ctl, e->name, KWABI_SLRU_BUFFERS, 0,
                          &e->state->lock, dir, tranche_id,
                          SYNC_HANDLER_NONE);
        }
#endif
    }

    kwabi_slru_initialised = (kwabi_slru_count > 0);
}

/* ── hook installation ─────────────────────────────────────────────────── */

bool
shim_slru_install_hooks(void)
{
    if (!process_shared_preload_libraries_in_progress)
    {
        /* A plain LOAD. SLRU is unavailable; the slots stay wired but every
         * call reports "requires preload" rather than pretending to work. */
        kwabi_slru_preloaded = false;
        return false;
    }

    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = kwabi_shmem_request;

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = kwabi_shmem_startup;

    /* The GUC must exist before the request hook reads it. */
    kwabi_slru_define_guc();

    kwabi_slru_preloaded = true;
    return true;
}

bool
shim_slru_is_available(void)
{
    return kwabi_slru_preloaded && kwabi_slru_initialised;
}

/* ── the ABI slots ─────────────────────────────────────────────────────── */

/*
 * slru_create — resolve a declared SLRU by name.
 *
 * NOT an initialisation. The postmaster already initialised every declared
 * SLRU at startup and this backend inherited the ctl through fork. So the only
 * honest thing this slot can do is validate that the requested name was
 * declared, and raise a clear error if the runtime is not preloaded at all.
 *
 * `nblocks` / `nslots` are accepted for ABI-shape compatibility but cannot be
 * honoured: the size was fixed at preload. They are checked against the
 * declared size and rejected on mismatch, so a caller that expects a different
 * size learns immediately rather than silently getting 16 slots.
 */
static void
shim_slru_create(const char *name, int nblocks, int nslots)
{
    KwabiSlruEntry *e;

    if (name == NULL)
        ereport(ERROR,
                (errmsg("kwabi: slru_create requires a name")));

    if (!kwabi_slru_preloaded)
        ereport(ERROR,
                (errmsg("kwabi: SLRU requires the runtime in shared_preload_libraries"),
                 errdetail("slru_create was called but the runtime was loaded with LOAD, "
                           "so no shared memory was reserved before fork."),
                 errhint("Add the runtime to shared_preload_libraries and set kwabi.slrus.")));

    e = kwabi_slru_lookup(name);
    if (e == NULL)
        ereport(ERROR,
                (errmsg("kwabi: SLRU \"%s\" was not declared", name),
                 errdetail("Declared SLRUs come from the %s GUC, read at preload.",
                           KWABI_SLRU_GUC)));

    if (nslots > 0 && nslots != KWABI_SLRU_BUFFERS)
        ereport(ERROR,
                (errmsg("kwabi: SLRU \"%s\" was declared with %d buffers, not %d",
                        name, KWABI_SLRU_BUFFERS, nslots),
                 errdetail("The buffer count is fixed at preload; it cannot be "
                           "chosen per call.")));

    (void) nblocks;
}

/*
 * slru_read — copy one page out of the SLRU into the caller's buffer.
 *
 * The lock discipline follows PostgreSQL's own test_slru: acquire the bank
 * lock (17/18) or the control lock (16) exclusively around the page lookup,
 * and copy out while still holding it.
 */
static void
shim_slru_read(const char *name, int64 pageno, void *data)
{
    KwabiSlruEntry *e = kwabi_slru_lookup(name);
    int             slotno;

    if (e == NULL || data == NULL)
        return;

#if PG_VERSION_NUM >= 170000
    LWLock *lock = SimpleLruGetBankLock(&e->ctl, pageno);
    LWLockAcquire(lock, LW_EXCLUSIVE);
#else
    LWLock *lock = &e->state->lock;
    LWLockAcquire(lock, LW_EXCLUSIVE);
#endif

    slotno = SimpleLruReadPage(&e->ctl, pageno, true, InvalidTransactionId);
    memcpy(data, e->ctl.shared->page_buffer[slotno], BLCKSZ);

    LWLockRelease(lock);
}

/*
 * slru_write — copy the caller's page into the SLRU and mark it dirty.
 *
 * The page is zeroed on first touch (SimpleLruZeroPage) and written out
 * (SimpleLruWritePage), which is the same sequence PostgreSQL's test_slru
 * uses for a write.
 */
static void
shim_slru_write(const char *name, int64 pageno, const void *data)
{
    KwabiSlruEntry *e = kwabi_slru_lookup(name);
    int             slotno;

    if (e == NULL || data == NULL)
        return;

#if PG_VERSION_NUM >= 170000
    LWLock *lock = SimpleLruGetBankLock(&e->ctl, pageno);
    LWLockAcquire(lock, LW_EXCLUSIVE);
#else
    LWLock *lock = &e->state->lock;
    LWLockAcquire(lock, LW_EXCLUSIVE);
#endif

    slotno = SimpleLruZeroPage(&e->ctl, pageno);
    memcpy(e->ctl.shared->page_buffer[slotno], data, BLCKSZ);
    e->ctl.shared->page_dirty[slotno] = true;
    e->ctl.shared->page_status[slotno] = SLRU_PAGE_VALID;
    SimpleLruWritePage(&e->ctl, slotno);

    LWLockRelease(lock);
}

/* ── init ──────────────────────────────────────────────────────────────── */

void
init_group_slru(void)
{
    shim_table.slru_create = shim_slru_create;
    shim_table.slru_read   = shim_slru_read;
    shim_table.slru_write  = shim_slru_write;
}

/* ── SQL-callable test functions ───────────────────────────────────────── */

/*
 * Every function below reaches the SLRU group THROUGH the published ABI table
 * (shim_api->slru_*), never through the static functions directly. That is the
 * point: the test must prove the slot is reachable and callable the way an
 * extension would call it, not that the C is internally consistent.
 */

/* The name the harness declares in kwabi.slrus. */
#define KWABI_SLRU_TEST_NAME "kwabitest"

PG_FUNCTION_INFO_V1(kwabi_slru_create_test);

/*
 * kwabi_slru_create_test() -> text
 *
 * Attach to the declared SLRU through the ABI. Returns "attached" on success;
 * raises if the slot is missing or the runtime is not preloaded (the latter is
 * the honest failure for a LOAD-only server).
 */
Datum
kwabi_slru_create_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL)
        ereport(ERROR, (errmsg("kwabi: ABI not initialised")));

    if (shim_api->slru_create == NULL)
        ereport(ERROR, (errmsg("kwabi: slru_create slot is not wired")));

    /* This raises with a clear message if not preloaded or not declared. */
    shim_api->slru_create(KWABI_SLRU_TEST_NAME, 0, 0);

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("kwabi: attached to SLRU \"%s\"", KWABI_SLRU_TEST_NAME)));
}

PG_FUNCTION_INFO_V1(kwabi_slru_write_test);

/*
 * kwabi_slru_write_test(pageno int8, data text) -> bool
 *
 * Write `data` into a page through the ABI. The page is BLCKSZ; the text is
 * copied in NUL-terminated, so a read of the same page returns the same text.
 */
Datum
kwabi_slru_write_test(PG_FUNCTION_ARGS)
{
    int64  pageno = PG_GETARG_INT64(0);
    char  *data   = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char   page[BLCKSZ];

    if (shim_api == NULL || shim_api->slru_write == NULL)
        ereport(ERROR, (errmsg("kwabi: slru_write slot is not wired")));

    memset(page, 0, sizeof(page));
    strlcpy(page, data, sizeof(page));

    shim_api->slru_write(KWABI_SLRU_TEST_NAME, pageno, page);

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(kwabi_slru_read_test);

/*
 * kwabi_slru_read_test(pageno int8) -> text
 *
 * Read a page back through the ABI and return it as text.
 */
Datum
kwabi_slru_read_test(PG_FUNCTION_ARGS)
{
    int64 pageno = PG_GETARG_INT64(0);
    char  page[BLCKSZ];

    if (shim_api == NULL || shim_api->slru_read == NULL)
        ereport(ERROR, (errmsg("kwabi: slru_read slot is not wired")));

    memset(page, 0, sizeof(page));
    shim_api->slru_read(KWABI_SLRU_TEST_NAME, pageno, page);
    page[sizeof(page) - 1] = '\0';   /* defensive: the writer NUL-terminates */

    PG_RETURN_TEXT_P(cstring_to_text(page));
}

PG_FUNCTION_INFO_V1(kwabi_slru_undeclared_test);

/*
 * kwabi_slru_undeclared_test() -> bool
 *
 * NEGATIVE CONTROL 1. Call slru_create for a name that was never declared.
 * That must RAISE. If this returns a row, the declaration check is vacuous.
 */
Datum
kwabi_slru_undeclared_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->slru_create == NULL)
        ereport(ERROR, (errmsg("kwabi: slru_create slot is not wired")));

    shim_api->slru_create("definitely_not_declared", 0, 0);

    /* Not reached if the check works. */
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(kwabi_slru_badcount_test);

/*
 * kwabi_slru_badcount_test() -> bool
 *
 * NEGATIVE CONTROL 2. Call slru_create for the declared name but with a
 * buffer count that does not match the preload-time size. That must RAISE.
 */
Datum
kwabi_slru_badcount_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->slru_create == NULL)
        ereport(ERROR, (errmsg("kwabi: slru_create slot is not wired")));

    shim_api->slru_create(KWABI_SLRU_TEST_NAME, 0, 9999);

    /* Not reached if the check works. */
    PG_RETURN_BOOL(true);
}

