/* group_explain.c — explain proof functions for the kwabi shim */

#include "shim_internal.h"

/* shim_internal.h defines QueryEnvironment as void * for the ABI typing. Here
 * the PostgreSQL struct is needed, so drop the macro for this file. */
#undef QueryEnvironment
#include "tcop/dest.h"               /* CreateDestReceiver, DestNone */
#include "tcop/tcopprot.h"           /* pg_plan_query */
#include "executor/execdesc.h"       /* CreateQueryDesc, FreeQueryDesc */
#include "executor/spi.h"            /* SPI_connect, SPI_execute */
#include "utils/memutils.h"          /* AllocSetContextCreate, GetMemoryChunkContext */

/* ========================================================================
 * Explain proof functions
 * ======================================================================== */

/*
 * shim_explain_get_index_name - get an index name through the ABI
 *
 * Result is assigned inside PG_TRY and returned AFTER PG_END_TRY. Returning
 * from between the two would skip the restore of PG_exception_stack and crash
 * the next ereport. See group_extension.c for the full note.
 */
static const char *
shim_explain_get_index_name(Oid indexOid)
{
    const char *result = NULL;

    if (!OidIsValid(indexOid))
        return NULL;

    PG_TRY();
    {
        Relation indexRel = relation_open(indexOid, AccessShareLock);
        result = pstrdup(RelationGetRelationName(indexRel));
        relation_close(indexRel, AccessShareLock);
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
 * kwabi_explain_get_index_name_test(int4) -> text
 *
 * Test explain_get_index_name through the ABI.
 */
PG_FUNCTION_INFO_V1(kwabi_explain_get_index_name_test);

Datum
kwabi_explain_get_index_name_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->explain_get_index_name == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_get_index_name is not wired")));

    Oid indexOid = (Oid) PG_GETARG_INT32(0);
    const char *name = shim_api->explain_get_index_name(indexOid);

    PG_RETURN_TEXT_P(cstring_to_text(name ? name : "(null)"));
}

/*
 * kwabi_explain_control() -> bool
 *
 * The NEGATIVE CONTROL. It asserts a wrong index name, which must fail.
 */
PG_FUNCTION_INFO_V1(kwabi_explain_control);

Datum
kwabi_explain_control(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->explain_get_index_name == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_get_index_name is not wired")));

    const char *name = shim_api->explain_get_index_name(1259); /* pg_class */

    /* The control's whole point: this comparison must be FALSE. */
    if (name != NULL && strcmp(name, "wrong_index_name") == 0)
        PG_RETURN_BOOL(true);   /* the comparison thinks pg_class is wrong_index_name: broken */

    ereport(ERROR,
            (errmsg("kwabi: explain negative control fired as intended"),
             errdetail("index name is not wrong_index_name -- the value comparison is honest")));
}


/* ========================================================================
 * Explain slots: ExplainState handles and explain_query
 *
 * Each ExplainState lives in its own memory context, a child of whichever
 * context created it. explain_state_free deletes that context, so the handle
 * owns everything it allocated.
 * ======================================================================== */

static KwabiExplainState
shim_explain_state_new(void)
{
    MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
                                              "kwabi explain state",
                                              ALLOCSET_SMALL_SIZES);
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ExplainState *es = NewExplainState();

    MemoryContextSwitchTo(old);
    return (KwabiExplainState) es;
}

static void
shim_explain_state_free(KwabiExplainState handle)
{
    ExplainState *es = (ExplainState *) handle;

    if (es == NULL)
        return;
    MemoryContextDelete(GetMemoryChunkContext(es));
}

static void
shim_explain_state_set_option(KwabiExplainState handle, const char *name, bool value)
{
    ExplainState *es = (ExplainState *) handle;

    if (es == NULL || name == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_state_set_option needs a state and a name")));

    if (strcmp(name, "verbose") == 0)       es->verbose = value;
    else if (strcmp(name, "costs") == 0)    es->costs = value;
    else if (strcmp(name, "buffers") == 0)  es->buffers = value;
    else if (strcmp(name, "wal") == 0)      es->wal = value;
    else if (strcmp(name, "timing") == 0)   es->timing = value;
    else if (strcmp(name, "summary") == 0)  es->summary = value;
    else if (strcmp(name, "analyze") == 0)  es->analyze = value;
#if PG_VERSION_NUM >= 170000
    else if (strcmp(name, "memory") == 0)   es->memory = value;
#endif
#if PG_VERSION_NUM >= 180000
    else if (strcmp(name, "settings") == 0) es->settings = value;
    else if (strcmp(name, "generic") == 0)  es->generic = value;
#endif
    else
        ereport(ERROR, (errmsg("kwabi: unknown EXPLAIN option \"%s\"", name)));
}

static void
shim_explain_state_set_format(KwabiExplainState handle, int format)
{
    ExplainState *es = (ExplainState *) handle;

    if (es == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_state_set_format needs a state")));
    if (format < EXPLAIN_FORMAT_TEXT || format > EXPLAIN_FORMAT_YAML)
        ereport(ERROR, (errmsg("kwabi: unknown EXPLAIN format %d", format)));
    es->format = (ExplainFormat) format;
}

static const char *
shim_explain_state_text(KwabiExplainState handle)
{
    ExplainState *es = (ExplainState *) handle;

    if (es == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_state_text needs a state")));
    return es->str->data;
}

static void
shim_explain_query(KwabiQueryDesc qd, KwabiIntoClause into, KwabiExplainState handle,
                   const char *queryString, KwabiParamListInfo params,
                   KwabiQueryEnvironment queryEnv)
{
    QueryDesc   *desc = (QueryDesc *) qd;
    ExplainState *es = (ExplainState *) handle;

    if (desc == NULL || es == NULL)
        ereport(ERROR, (errmsg("kwabi: explain_query needs a query and a state")));

#if PG_VERSION_NUM >= 170000
    ExplainOnePlan(desc->plannedstmt, (IntoClause *) into, es, queryString,
                   (ParamListInfo) params, (struct QueryEnvironment *) queryEnv,
                   NULL, NULL, NULL);
#else
    ExplainOnePlan(desc->plannedstmt, (IntoClause *) into, es, queryString,
                   (ParamListInfo) params, (struct QueryEnvironment *) queryEnv,
                   NULL, NULL);
#endif
}

void
init_group_explain(void)
{
    shim_table.explain_get_index_name = shim_explain_get_index_name;
    shim_table.explain_query = shim_explain_query;
    shim_table.explain_state_new = shim_explain_state_new;
    shim_table.explain_state_set_option = shim_explain_state_set_option;
    shim_table.explain_state_set_format = shim_explain_state_set_format;
    shim_table.explain_state_text = shim_explain_state_text;
    shim_table.explain_state_free = shim_explain_state_free;
}

/* ---- proof functions ------------------------------------------------- */

/*
 * explain_abi_matches_sql(sql, analyze) -> bool
 *
 * Plan sql. EXPLAIN it through the ABI, and through SQL with the same options.
 * The two texts must be byte-identical. Timing and the summary are off on both
 * sides, so the text is deterministic. BUFFERS is off too: PostgreSQL 18 turns
 * it on by default under ANALYZE, while a fresh ExplainState starts from the C
 * defaults (off). Callers must set options explicitly; the ABI does not apply
 * SQL's defaults. With analyze, the plan runs on both
 * sides, and the row counts must agree too.
 */
static bool
explain_abi_matches_sql(const char *sql, bool analyze)
{
    if (shim_api == NULL || shim_api->explain_query == NULL ||
        shim_api->explain_state_new == NULL || shim_api->explain_state_text == NULL ||
        shim_api->explain_state_free == NULL || shim_api->explain_state_set_option == NULL)
        ereport(ERROR, (errmsg("kwabi: explain slots are not wired")));

    KwabiNode query = shim_api->parse_stmt(sql);
    if (query == NULL)
        ereport(ERROR, (errmsg("kwabi: parse_stmt returned NULL")));

    PlannedStmt *pstmt = pg_plan_query((Query *) query, sql, 0, NULL);
    QueryDesc   *qd = CreateQueryDesc(pstmt, sql, GetActiveSnapshot(), InvalidSnapshot,
                                      CreateDestReceiver(DestNone), NULL, NULL, 0);

    KwabiExplainState es = shim_api->explain_state_new();
    shim_api->explain_state_set_option(es, "costs", false);
    shim_api->explain_state_set_option(es, "timing", false);
    shim_api->explain_state_set_option(es, "summary", false);
    shim_api->explain_state_set_option(es, "buffers", false);
    shim_api->explain_state_set_option(es, "analyze", analyze);
    shim_api->explain_query((KwabiQueryDesc) qd, NULL, es, sql, NULL, NULL);
    char *from_abi = pstrdup(shim_api->explain_state_text(es));
    shim_api->explain_state_free(es);
    FreeQueryDesc(qd);

    /* The SQL side, row by row. The buffer is made before SPI connects so
     * SPI_finish does not free it. */
    StringInfoData from_sql;
    initStringInfo(&from_sql);

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute(psprintf("EXPLAIN (COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF%s) %s",
                             analyze ? ", ANALYZE" : "", sql), false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: EXPLAIN via SPI failed")));
    }
    for (uint64 i = 0; i < SPI_processed; i++) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
        appendStringInfo(&from_sql, "%s\n", TextDatumGetCString(d));
    }
    SPI_finish();

    if (strcmp(from_abi, from_sql.data) != 0)
        ereport(NOTICE, (errmsg("explain mismatch (analyze=%d)", (int) analyze),
                         errdetail("abi: [%s] sql: [%s]", from_abi, from_sql.data)));
    return strcmp(from_abi, from_sql.data) == 0;
}

/*
 * kwabi_explain_matches_sql_test(sql) -> bool
 *
 * EXPLAIN text through the ABI equals SQL EXPLAIN, without ANALYZE.
 */
PG_FUNCTION_INFO_V1(kwabi_explain_matches_sql_test);

Datum
kwabi_explain_matches_sql_test(PG_FUNCTION_ARGS)
{
    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool ok = explain_abi_matches_sql(sql, false);

    pfree(sql);
    PG_RETURN_BOOL(ok);
}

/*
 * kwabi_explain_analyze_matches_sql_test(sql) -> bool
 *
 * EXPLAIN ANALYZE through the ABI runs the plan and matches SQL EXPLAIN ANALYZE
 * byte for byte (actual row counts included).
 */
PG_FUNCTION_INFO_V1(kwabi_explain_analyze_matches_sql_test);

Datum
kwabi_explain_analyze_matches_sql_test(PG_FUNCTION_ARGS)
{
    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool ok = explain_abi_matches_sql(sql, true);

    pfree(sql);
    PG_RETURN_BOOL(ok);
}

/* 1 if the option is accepted, 0 if it raises. Any other error is rethrown. */
static bool
explain_option_accepted(KwabiExplainState es, const char *name)
{
    bool accepted = true;

    PG_TRY();
    {
        shim_api->explain_state_set_option(es, name, true);
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();

        if (strstr(edata->message, "unknown EXPLAIN option") == NULL)
        {
            PG_RE_THROW();
        }
        accepted = false;
        FreeErrorData(edata);
        FlushErrorState();
    }
    PG_END_TRY();

    return accepted;
}

/*
 * kwabi_explain_option_version_test() -> bool
 *
 * The options that exist only on some majors must be accepted exactly there:
 * memory on 17 and later, settings and generic on 18. Elsewhere they raise.
 */
PG_FUNCTION_INFO_V1(kwabi_explain_option_version_test);

Datum
kwabi_explain_option_version_test(PG_FUNCTION_ARGS)
{
    KwabiExplainState es = shim_api->explain_state_new();
    bool memory_ok, settings_ok, generic_ok;

    memory_ok = explain_option_accepted(es, "memory");
    settings_ok = explain_option_accepted(es, "settings");
    generic_ok = explain_option_accepted(es, "generic");
    shim_api->explain_state_free(es);

    ereport(NOTICE, (errmsg("explain option versions: memory=%d settings=%d generic=%d (PG %d)",
                            (int) memory_ok, (int) settings_ok, (int) generic_ok, PG_VERSION_NUM)));
#if PG_VERSION_NUM >= 180000
    PG_RETURN_BOOL(memory_ok && settings_ok && generic_ok);
#elif PG_VERSION_NUM >= 170000
    PG_RETURN_BOOL(memory_ok && !settings_ok && !generic_ok);
#else
    PG_RETURN_BOOL(!memory_ok && !settings_ok && !generic_ok);
#endif
}

static void call_explain_bad_option(void)
{
    KwabiExplainState es = shim_api->explain_state_new();
    shim_api->explain_state_set_option(es, "no_such_option", true);
}

/*
 * kwabi_explain_bad_option_test() -> bool
 *
 * An unknown option must raise "unknown EXPLAIN option".
 */
PG_FUNCTION_INFO_V1(kwabi_explain_bad_option_test);

Datum
kwabi_explain_bad_option_test(PG_FUNCTION_ARGS)
{
    bool ok = false;

    if (shim_api == NULL || shim_api->explain_state_set_option == NULL)
        ereport(ERROR, (errmsg("kwabi: explain slots are not wired")));

    PG_TRY();
    {
        call_explain_bad_option();
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        ok = (strstr(edata->message, "unknown EXPLAIN option") != NULL);
        FreeErrorData(edata);
        FlushErrorState();
    }
    PG_END_TRY();

    PG_RETURN_BOOL(ok);
}
