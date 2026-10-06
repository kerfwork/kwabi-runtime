/* group_defrem.c — defrem slots for the kwabi shim */

#include "shim_internal.h"

static bool
split_table_column(const char *name, char *table, size_t table_len,
                   char *column, size_t column_len)
{
    const char *dot;

    if (name == NULL || table == NULL || column == NULL)
        return false;

    dot = strrchr(name, '.');
    if (dot == NULL)
        return false;

    if ((size_t)(dot - name) >= table_len)
        return false;
    if (strlen(dot + 1) >= column_len)
        return false;

    snprintf(table, table_len, "%.*s", (int)(dot - name), name);
    snprintf(column, column_len, "%s", dot + 1);
    return true;
}

static void
shim_defrem_create(const char *name, const char *type, const char *value)
{
    char         sql[1024];
    char         table[256];
    char         column[256];

    if (name == NULL || type == NULL || value == NULL)
        return;

    if (!split_table_column(name, table, sizeof(table), column, sizeof(column)))
        return;

    snprintf(sql, sizeof(sql),
             "ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
             table, column, value);

    if (SPI_connect() != SPI_OK_CONNECT)
        return;

    if (SPI_execute(sql, false, 0) < 0) {
        SPI_finish();
        return;
    }

    SPI_finish();
}

static void
shim_defrem_alter(const char *name, const char *value)
{
    char         sql[1024];
    char         table[256];
    char         column[256];

    if (name == NULL || value == NULL)
        return;

    if (!split_table_column(name, table, sizeof(table), column, sizeof(column)))
        return;

    snprintf(sql, sizeof(sql),
             "ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
             table, column, value);

    if (SPI_connect() != SPI_OK_CONNECT)
        return;

    if (SPI_execute(sql, false, 0) < 0) {
        SPI_finish();
        return;
    }

    SPI_finish();
}

static void
shim_defrem_drop(const char *name)
{
    char         sql[1024];
    char         table[256];
    char         column[256];

    if (name == NULL)
        return;

    if (!split_table_column(name, table, sizeof(table), column, sizeof(column)))
        return;

    snprintf(sql, sizeof(sql),
             "ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT",
             table, column);

    if (SPI_connect() != SPI_OK_CONNECT)
        return;

    if (SPI_execute(sql, false, 0) < 0) {
        SPI_finish();
        return;
    }

    SPI_finish();
}

void
init_group_defrem(void)
{
    shim_table.defrem_create = shim_defrem_create;
    shim_table.defrem_alter = shim_defrem_alter;
    shim_table.defrem_drop = shim_defrem_drop;
}
