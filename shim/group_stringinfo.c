/* group_stringinfo.c — stringinfo slots for the kwabi shim */

#include "shim_internal.h"

static void
shim_stringinfo_init(StringInfo str)
{
    if (str == NULL)
        return;
    PG_TRY();
    {
        initStringInfo(str);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_stringinfo_reset(StringInfo str)
{
    if (str == NULL)
        return;
    PG_TRY();
    {
        resetStringInfo(str);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_stringinfo_append(StringInfo str, const char *data)
{
    if (str == NULL || data == NULL)
        return;
    PG_TRY();
    {
        appendStringInfoString(str, data);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_stringinfo_append_char(StringInfo str, char c)
{
    if (str == NULL)
        return;
    PG_TRY();
    {
        appendStringInfoChar(str, c);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void
shim_stringinfo_append_int(StringInfo str, int64 value)
{
    if (str == NULL)
        return;
    PG_TRY();
    {
        appendStringInfo(str, "%ld", (long) value);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static const char *
shim_stringinfo_data(StringInfo str)
{
    if (str == NULL)
        return NULL;
    return str->data;
}

static int
shim_stringinfo_len(StringInfo str)
{
    if (str == NULL)
        return 0;
    return str->len;
}

void
init_group_stringinfo(void)
{
    shim_table.stringinfo_init = shim_stringinfo_init;
    shim_table.stringinfo_reset = shim_stringinfo_reset;
    shim_table.stringinfo_append = shim_stringinfo_append;
    shim_table.stringinfo_append_char = shim_stringinfo_append_char;
    shim_table.stringinfo_append_int = shim_stringinfo_append_int;
    shim_table.stringinfo_data = shim_stringinfo_data;
    shim_table.stringinfo_len = shim_stringinfo_len;
}
