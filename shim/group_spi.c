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
    shim_table.spi_free_result = shim_spi_free_result;
    shim_table.spi_result_ntuples = shim_spi_result_ntuples;
    shim_table.spi_result_get_value = shim_spi_result_get_value;
}
