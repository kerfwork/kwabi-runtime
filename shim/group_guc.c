/* group_guc.c — GUC slots for the kwabi shim */

#include "shim_internal.h"

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

void
init_group_guc(void)
{
    shim_table.guc_get_int = shim_guc_get_int;
    shim_table.guc_get_string = shim_guc_get_string;
    shim_table.guc_get_bool = shim_guc_get_bool;
    shim_table.guc_get_float = shim_guc_get_float;
    shim_table.guc_set_int = shim_guc_set_int;
    shim_table.guc_set_string = shim_guc_set_string;
    shim_table.guc_set_bool = shim_guc_set_bool;
    shim_table.guc_set_float = shim_guc_set_float;
}
