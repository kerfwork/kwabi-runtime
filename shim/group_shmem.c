/* group_shmem.c — shmem slots for the kwabi shim */

#include "shim_internal.h"

static void *
shim_shmem_alloc(size_t size)
{
    void *result = NULL;

    PG_TRY();
    {
        result = ShmemAlloc(size);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

static void
shim_shmem_free(void *pointer)
{
    if (pointer == NULL)
        return;

    PG_TRY();
    {
        pfree(pointer);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static void *
shim_shmem_get(const char *name, size_t size)
{
    void *result = NULL;
    bool found;

    if (name == NULL)
        return NULL;

    PG_TRY();
    {
        result = ShmemInitStruct(name, size, &found);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

void
init_group_shmem(void)
{
    shim_table.shmem_alloc = shim_shmem_alloc;
    shim_table.shmem_free = shim_shmem_free;
    shim_table.shmem_get = shim_shmem_get;
}
