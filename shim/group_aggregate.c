/* group_aggregate.c — SQL-callable aggregate functions for bound aggregate bodies.
 *
 * A library bound under a name exports kwabi_aggregate_bodies() (see KwabiAggBodies in
 * kwabi.h). An aggregate is created from these functions, all on the same six C symbols:
 *
 *     CREATE AGGREGATE <agg>(T) (SFUNC = <b>__step, STYPE = internal,
 *                                FINALFUNC = <b>__final, ...)
 *
 * where <b> is the binding name. The binding is the part of the function's name before
 * "__"; the role (step, final, ...) is fixed by which symbol the function uses.
 *
 * The state is an KwabiAggState: the body's opaque state plus a copy of the body table that
 * created it (pinning). Every later call uses that copy, never the currently bound table,
 * so a reload during an aggregate cannot change its layout. Combine refuses two states
 * whose pinned tables are different (0A000).
 *
 * Nulls are skipped here, before the body sees them. Errors from the body are raised
 * through the same path as the other bodies.
 */

#include "shim_internal.h"
#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "utils/lsyscache.h"

#define KWABI_AGG_STATE_MAGIC 0x4B414747u     /* "KAGG" */
#define KWABI_AGG_BLOB_MAX    (16 * 1024 * 1024)

typedef struct KwabiAggState
{
    uint32      magic;
    KwabiAggBodies pinned;              /* the table that created this state */
    void       *state;                  /* the body's state; opaque here */
} KwabiAggState;

/* The binding and role of the running function, from its name "<binding>__<role>". */
static char *
split_binding(Oid fn_oid, const char **role)
{
    char       *fname = get_func_name(fn_oid);
    char       *sep;

    if (fname == NULL)
        ereport(ERROR, (errmsg("kwabi: cannot find the name of the running aggregate function")));
    /* the last "__": a binding name may itself contain single underscores */
    sep = NULL;
    for (char *p = fname; (p = strstr(p, "__")) != NULL; p += 2)
        sep = p;
    if (sep == NULL || sep == fname || sep[2] == '\0')
        ereport(ERROR, (errmsg("kwabi: aggregate function %s has no __<role> suffix", fname)));
    *role = sep + 2;
    return pnstrdup(fname, sep - fname);
}

static KwabiAggState *
check_state(void *p)
{
    KwabiAggState   *st = (KwabiAggState *) p;

    if (st == NULL || st->magic != KWABI_AGG_STATE_MAGIC)
        ereport(ERROR, (errmsg("kwabi: aggregate state is not from a kwabi aggregate")));
    return st;
}

static void
raise_if_failed(KwabiStatus st, KwabiError *err)
{
    if (st != KWABI_OK)
    {
        if (err->sqlerrcode == 0)
            kwabi_error_set_core(err, ERRCODE_INTERNAL_ERROR, KWABI_ERR_BODY_RAISED,
                                 "aggregate body failed");
        shim_raise_kwabi_error(err);
    }
}

/* Two tables are the same body image when their entry points are the same. */
static bool
same_body(const KwabiAggBodies *a, const KwabiAggBodies *b)
{
    return a->init == b->init && a->step == b->step && a->arg == b->arg;
}

/* ---- step (internal, T) -> internal ---- */

PG_FUNCTION_INFO_V1(kwabi_agg_step);
Datum
kwabi_agg_step(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;
    KwabiAggState   *st = PG_ARGISNULL(0) ? NULL : (KwabiAggState *) PG_GETARG_POINTER(0);
    KwabiError  err;
    KwabiStatus s;

    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR, (errmsg("kwabi aggregate step called outside an aggregate")));

    if (PG_ARGISNULL(1))
    {
        /* Nulls are skipped; the state stays as it is, which may be none yet. */
        if (st == NULL)
            PG_RETURN_NULL();
        PG_RETURN_POINTER(st);
    }

    if (st == NULL)
    {
        const char *role;
        char       *name = split_binding(fcinfo->flinfo->fn_oid, &role);
        KwabiAggBodies table;
        void       *body_state = NULL;
        MemoryContext oldctx;

        if (!shim_agg_bodies_lookup(name, &table))
            ereport(ERROR, (errmsg("kwabi aggregate \"%s\" is not bound to a library with an aggregate table",
                                   name)));

        /* The state, and anything the body allocates in init, lives in the aggregate context. */
        oldctx = MemoryContextSwitchTo(aggctx);
        st = (KwabiAggState *) palloc0(sizeof(KwabiAggState));
        st->magic = KWABI_AGG_STATE_MAGIC;
        st->pinned = table;
        kwabi_error_init(&err);
        s = table.init(&body_state, &err, table.arg);
        MemoryContextSwitchTo(oldctx);
        raise_if_failed(s, &err);
        st->state = body_state;
    }
    check_state(st);

    kwabi_error_init(&err);
    s = st->pinned.step(st->state, (uint64) PG_GETARG_INT64(1), &err, st->pinned.arg);
    raise_if_failed(s, &err);
    PG_RETURN_POINTER(st);
}

/* ---- final (internal) -> T ---- */

PG_FUNCTION_INFO_V1(kwabi_agg_final);
Datum
kwabi_agg_final(PG_FUNCTION_ARGS)
{
    KwabiAggState   *st;
    uint64      result = 0;
    KwabiError  err;
    KwabiStatus s;

    /* No input at all: the result is NULL. */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    st = check_state(PG_GETARG_POINTER(0));

    kwabi_error_init(&err);
    s = st->pinned.final(st->state, &result, &err, st->pinned.arg);
    raise_if_failed(s, &err);
    PG_RETURN_INT64((int64) result);
}

/* ---- inverse (internal, T) -> internal: moving frames ---- */

PG_FUNCTION_INFO_V1(kwabi_agg_inverse);
Datum
kwabi_agg_inverse(PG_FUNCTION_ARGS)
{
    KwabiAggState   *st = check_state(PG_ARGISNULL(0) ? NULL : PG_GETARG_POINTER(0));
    KwabiError  err;
    KwabiStatus s;

    if (st->pinned.inverse == NULL)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("kwabi aggregate has no inverse body")));
    if (PG_ARGISNULL(1))
        PG_RETURN_POINTER(st);

    kwabi_error_init(&err);
    s = st->pinned.inverse(st->state, (uint64) PG_GETARG_INT64(1), &err, st->pinned.arg);
    raise_if_failed(s, &err);
    PG_RETURN_POINTER(st);
}

/* ---- combine (internal, internal) -> internal: partial aggregation ---- */

PG_FUNCTION_INFO_V1(kwabi_agg_combine);
Datum
kwabi_agg_combine(PG_FUNCTION_ARGS)
{
    KwabiAggState   *a = PG_ARGISNULL(0) ? NULL : (KwabiAggState *) PG_GETARG_POINTER(0);
    KwabiAggState   *b = PG_ARGISNULL(1) ? NULL : (KwabiAggState *) PG_GETARG_POINTER(1);
    KwabiError  err;
    KwabiStatus s;

    if (a == NULL && b == NULL)
        PG_RETURN_NULL();
    if (a == NULL)
        PG_RETURN_POINTER(check_state(b));
    if (b == NULL)
        PG_RETURN_POINTER(check_state(a));

    check_state(a);
    check_state(b);
    if (!same_body(&a->pinned, &b->pinned))
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("aggregate states from different body versions cannot be combined")));
    if (a->pinned.combine == NULL)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("kwabi aggregate has no combine body")));

    kwabi_error_init(&err);
    s = a->pinned.combine(a->state, b->state, &err, a->pinned.arg);
    raise_if_failed(s, &err);
    PG_RETURN_POINTER(a);
}

/* ---- serialize (internal) -> bytea: parallel workers send partial states ---- */

PG_FUNCTION_INFO_V1(kwabi_agg_serialize);
Datum
kwabi_agg_serialize(PG_FUNCTION_ARGS)
{
    KwabiAggState   *st;
    size_t      cap = 256, used = 0;
    char       *buf;
    bytea      *out;
    KwabiError  err;
    KwabiStatus s;
    uint32      version;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    st = check_state(PG_GETARG_POINTER(0));
    if (st->pinned.serialize == NULL || st->pinned.deserialize == NULL)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("kwabi aggregate has no serialize body")));

    buf = palloc(cap);
    for (;;)
    {
        kwabi_error_init(&err);
        s = st->pinned.serialize(st->state, buf, cap, &used, &err, st->pinned.arg);
        if (s == KWABI_OK)
            break;
        if (s == KWABI_ERR_BAD_ARG && cap < KWABI_AGG_BLOB_MAX)
        {
            cap *= 2;
            buf = repalloc(buf, cap);
            continue;
        }
        raise_if_failed(s, &err);
    }

    /* The blob is the body table's version, then the body's bytes. */
    version = st->pinned.version;
    out = (bytea *) palloc(VARHDRSZ + sizeof(uint32) + used);
    SET_VARSIZE(out, VARHDRSZ + sizeof(uint32) + used);
    memcpy(VARDATA(out), &version, sizeof(uint32));
    memcpy(VARDATA(out) + sizeof(uint32), buf, used);
    PG_RETURN_BYTEA_P(out);
}

/* ---- deserialize (bytea, internal) -> internal ---- */

PG_FUNCTION_INFO_V1(kwabi_agg_deserialize);
Datum
kwabi_agg_deserialize(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;
    const char *role;
    char       *name;
    KwabiAggBodies table;
    bytea      *in = PG_GETARG_BYTEA_PP(0);
    size_t      len = VARSIZE_ANY_EXHDR(in);
    uint32      version;
    void       *body_state = NULL;
    KwabiAggState   *st;
    MemoryContext oldctx;
    KwabiError  err;
    KwabiStatus s;

    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR, (errmsg("kwabi aggregate deserialize called outside an aggregate")));
    if (len < sizeof(uint32))
        ereport(ERROR, (errmsg("kwabi aggregate state blob is too short")));

    name = split_binding(fcinfo->flinfo->fn_oid, &role);
    if (!shim_agg_bodies_lookup(name, &table))
        ereport(ERROR, (errmsg("kwabi aggregate \"%s\" is not bound to a library with an aggregate table",
                               name)));

    memcpy(&version, VARDATA_ANY(in), sizeof(uint32));
    if (version != table.version)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("aggregate state from body version %u cannot be read by version %u",
                               version, table.version)));

    oldctx = MemoryContextSwitchTo(aggctx);
    st = (KwabiAggState *) palloc0(sizeof(KwabiAggState));
    st->magic = KWABI_AGG_STATE_MAGIC;
    st->pinned = table;
    kwabi_error_init(&err);
    s = table.deserialize((const char *) VARDATA_ANY(in) + sizeof(uint32),
                          len - sizeof(uint32), &body_state, &err, table.arg);
    MemoryContextSwitchTo(oldctx);
    raise_if_failed(s, &err);
    st->state = body_state;
    PG_RETURN_POINTER(st);
}
