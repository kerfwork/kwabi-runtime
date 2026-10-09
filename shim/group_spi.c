/* group_spi.c — SPI slots for the kwabi shim */

#include "shim_internal.h"

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

/*
 * A plan is kept past the SPI connection that prepared it, so the handle an
 * extension holds stays valid across calls until spi_free_plan.
 */
static KwabiSPIPlan
shim_spi_prepare(const char *sql, int nargs, Oid *argtypes)
{
    SPIPlanPtr plan;

    if (sql == NULL)
        ereport(ERROR, (errmsg("kwabi: spi_prepare needs a query")));

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));

    plan = SPI_prepare(sql, nargs, argtypes);
    if (plan == NULL || SPI_keepplan(plan) != 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: spi_prepare failed")));
    }

    SPI_finish();
    return (KwabiSPIPlan) plan;
}

static void
shim_spi_free_plan(KwabiSPIPlan plan)
{
    if (plan != NULL)
        SPI_freeplan((SPIPlanPtr) plan);
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

    pfree(result);    /* free while its context is still alive */
    SPI_finish();     /* then tear down SPI */
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

void
init_group_spi(void)
{
    shim_table.spi_execute = shim_spi_execute;
    shim_table.spi_execute_plan = shim_spi_execute_plan;
    shim_table.spi_prepare = shim_spi_prepare;
    shim_table.spi_free_plan = shim_spi_free_plan;
    shim_table.spi_free_result = shim_spi_free_result;
    shim_table.spi_result_ntuples = shim_spi_result_ntuples;
    shim_table.spi_result_get_value = shim_spi_result_get_value;
}

/* ---- proof function -------------------------------------------------- */

/*
 * kwabi_spi_plan_test() -> bool
 *
 * A plan made through the ABI, with one int4 parameter, must run with that
 * parameter and return the computed value. The plan must survive the SPI
 * connection that made it, so it is run after the prepare returns.
 */
PG_FUNCTION_INFO_V1(kwabi_spi_plan_test);

Datum
kwabi_spi_plan_test(PG_FUNCTION_ARGS)
{
    Oid        argtypes[1] = { INT4OID };
    Datum      values[1];
    KwabiSPIPlan plan;
    KwabiSPIResult result;
    bool       ok = false;

    if (shim_api == NULL || shim_api->spi_prepare == NULL ||
        shim_api->spi_execute_plan == NULL || shim_api->spi_free_plan == NULL ||
        shim_api->spi_result_ntuples == NULL || shim_api->spi_result_get_value == NULL ||
        shim_api->spi_free_result == NULL)
        ereport(ERROR, (errmsg("kwabi: spi plan slots are not wired")));

    plan = shim_api->spi_prepare("SELECT $1::int4 + 1", 1, argtypes);
    if (plan == NULL)
        ereport(ERROR, (errmsg("kwabi: spi_prepare returned NULL")));

    values[0] = Int32GetDatum(41);
    result = shim_api->spi_execute_plan(plan, values, " ", true, 0);
    if (result != NULL) {
        ok = (shim_api->spi_result_ntuples(result) == 1 &&
              DatumGetInt32(shim_api->spi_result_get_value(result, 0, 1)) == 42);
        shim_api->spi_free_result(result);
    }

    shim_api->spi_free_plan(plan);
    PG_RETURN_BOOL(ok);
}
