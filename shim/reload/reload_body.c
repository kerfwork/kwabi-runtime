/*
 * reload_body.c — a reloadable hook body for hook-reload.sql.
 *
 * Built twice with -DBODY_TAG='"1"' and -DBODY_TAG='"2"'. It refuses a planned
 * statement whose text contains kwt_reload, with a message that names its tag. The
 * SQL test reads the tag from the error, so no shared trace is needed.
 */
#include "kwabi.h"
#include <string.h>

#ifndef BODY_TAG
#error "build with -DBODY_TAG=\"1\" or \"2\""
#endif

static const KwabiV1 *api = NULL;

static KwabiStatus
planner_body(KwabiNode parse, const char *queryString, int cursorOptions,
             KwabiParamListInfo boundParams, KwabiHookNext next, KwabiNode *planned,
             KwabiError *err, void *arg)
{
    if (queryString != NULL && strstr(queryString, "kwt_reload") != NULL)
    {
        kwabi_error_init(err);
        kwabi_error_set_core(err, 0, KWABI_ERR_BODY_RAISED,
                             "reload body " BODY_TAG " refused the statement");
        return KWABI_ERR_BODY_RAISED;
    }
    return api->hook_next_planner(next, parse, queryString, cursorOptions,
                                  boundParams, planned, err);
}

bool
kwabi_ext_init(const KwabiV1 *table)
{
    api = table;
    return table != NULL;
}

static const KwabiHookBodies bodies = {
    .size = sizeof(KwabiHookBodies),
    .version = KWABI_HOOK_BODIES_VERSION,
    .planner = planner_body,
};

const KwabiHookBodies *
kwabi_hook_bodies(void)
{
    return &bodies;
}
