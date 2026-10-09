/* group_trigger.c — trigger slots for the kwabi shim */

#include "shim_internal.h"
#include "commands/trigger.h"        /* CopyTriggerDesc */
#include "executor/spi.h"           /* SPI_connect, SPI_execute */
#include "utils/builtins.h"         /* DatumGetObjectId */

/*
 * The trigger handles are opaque at the ABI. trigger_desc returns a COPY of
 * the relcache TriggerDesc, made in the caller's current memory context, so
 * the handle outlives the relation open/close and an ALTER or DROP of the
 * relation cannot leave it pointing at freed memory. A KwabiTrigger points
 * into that copy, so it lives as long as the desc.
 */

/* ---- trigger slots --------------------------------------------------- */

KwabiTriggerDesc
shim_trigger_desc(Oid relid)
{
    Relation rel = relation_open(relid, AccessShareLock);
    TriggerDesc *copy = CopyTriggerDesc(rel->trigdesc);
    relation_close(rel, AccessShareLock);
    return (KwabiTriggerDesc) copy;
}

int
shim_trigger_count(KwabiTriggerDesc handle)
{
    TriggerDesc *desc = (TriggerDesc *) handle;
    return desc == NULL ? 0 : desc->numtriggers;
}

KwabiTrigger
shim_trigger_get(KwabiTriggerDesc handle, int index)
{
    TriggerDesc *desc = (TriggerDesc *) handle;

    if (desc == NULL || index < 0 || index >= desc->numtriggers)
        return NULL;
    return (KwabiTrigger) &desc->triggers[index];
}

void
init_group_trigger(void)
{
    shim_table.trigger_desc = shim_trigger_desc;
    shim_table.trigger_count = shim_trigger_count;
    shim_table.trigger_get = shim_trigger_get;
}

/* ---- proof functions ------------------------------------------------- */

/*
 * A table with one trigger, built from PostgreSQL's own
 * suppress_redundant_updates_trigger so no trigger function is needed. The
 * table is dropped by kwabi_trigger_fixture_drop.
 */
#define KWABI_TRG_TABLE "kwabi_trg_t"
#define KWABI_TRG_NAME  "kwabi_trg"

static Oid
kwabi_trigger_fixture_create(void)
{
    Oid relid = InvalidOid;

    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE IF EXISTS " KWABI_TRG_TABLE, false, 0) < 0 ||
        SPI_execute("CREATE TABLE " KWABI_TRG_TABLE " (a int)", false, 0) < 0 ||
        SPI_execute("CREATE TRIGGER " KWABI_TRG_NAME " BEFORE UPDATE ON " KWABI_TRG_TABLE
                    " FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger()",
                    false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: trigger fixture setup failed")));
    }
    if (SPI_execute("SELECT oid FROM pg_class WHERE relname = '" KWABI_TRG_TABLE "'",
                    false, 0) < 0 || SPI_processed != 1) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: could not find trigger fixture table")));
    }
    bool isnull;
    relid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0],
                                           SPI_tuptable->tupdesc, 1, &isnull));
    SPI_finish();
    return relid;
}

static void
kwabi_trigger_fixture_drop(void)
{
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("kwabi: SPI_connect failed")));
    if (SPI_execute("DROP TABLE IF EXISTS " KWABI_TRG_TABLE, false, 0) < 0) {
        SPI_finish();
        ereport(ERROR, (errmsg("kwabi: DROP TABLE failed")));
    }
    SPI_finish();
}

/*
 * kwabi_trigger_desc_test() -> bool
 *
 * trigger_desc must return a non-NULL desc for a table with one trigger, and
 * trigger_count must report exactly one.
 */
PG_FUNCTION_INFO_V1(kwabi_trigger_desc_test);

Datum
kwabi_trigger_desc_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->trigger_desc == NULL ||
        shim_api->trigger_count == NULL)
        ereport(ERROR, (errmsg("kwabi: trigger slots are not wired")));

    Oid relid = kwabi_trigger_fixture_create();
    KwabiTriggerDesc desc = shim_api->trigger_desc(relid);
    bool ok = (desc != NULL && shim_api->trigger_count(desc) == 1);

    kwabi_trigger_fixture_drop();
    PG_RETURN_BOOL(ok);
}

/*
 * kwabi_trigger_get_test() -> bool
 *
 * trigger_get(desc, 0) must return the trigger by name, and an out-of-range
 * index must return NULL.
 */
PG_FUNCTION_INFO_V1(kwabi_trigger_get_test);

Datum
kwabi_trigger_get_test(PG_FUNCTION_ARGS)
{
    if (shim_api == NULL || shim_api->trigger_desc == NULL ||
        shim_api->trigger_get == NULL)
        ereport(ERROR, (errmsg("kwabi: trigger slots are not wired")));

    Oid relid = kwabi_trigger_fixture_create();
    KwabiTriggerDesc desc = shim_api->trigger_desc(relid);
    bool ok = false;

    if (desc != NULL) {
        KwabiTrigger first = shim_api->trigger_get(desc, 0);
        KwabiTrigger past = shim_api->trigger_get(desc, 1);

        ok = (first != NULL && past == NULL &&
              strcmp(((Trigger *) first)->tgname, KWABI_TRG_NAME) == 0);
    }

    kwabi_trigger_fixture_drop();
    PG_RETURN_BOOL(ok);
}
