/*
 * canary.c — a kwabi extension that links nothing from PostgreSQL.
 *
 * This is the build-once-run-everywhere claim, made checkable. The file
 * includes only kwabi.h. It does not include postgres.h, it does not link
 * against any PostgreSQL symbol, and it contains no `#if PG_VERSION_NUM`
 * branches. If the ABI works, this binary loads on 16, 17 and 18 unchanged.
 *
 * Build:  cc -shared -fPIC -I.. -o libcanary.dylib canary.c
 */

#include "kwabi.h"

#include <string.h>

/* The table handed to us at load time. A real extension would keep this in a
 * per-backend state block rather than a global. */
static const KwabiV1 *kwabi_api = NULL;

/* Counters so the harness can prove the calls arrived. */
static int allocs = 0;
static int frees = 0;

/*
 * Load-time entry point, called by the runtime.
 *
 * The version check is the contract: an extension must refuse a table it does
 * not understand rather than calling into it. Checking `version` alone is
 * enough today; once the table grows past v1 the extension will also compare
 * `size` so a newer extension running on an older runtime fails cleanly.
 */
bool
kwabi_ext_init(const KwabiV1 *api)
{
    if (api == NULL)
        return false;
    if (api->version != KWABI_VERSION)
        return false;

    kwabi_api = api;
    return true;
}

/*
 * Allocate a buffer through the ABI, write a marker into it, and return the
 * pointer. The harness reads the marker back to prove the memory really came
 * from the runtime's allocator.
 *
 * The marker is written byte by byte rather than with memcpy so that this
 * object has no undefined symbols at all: a canary that pulls in nothing from
 * libc is a canary whose only possible dependency is the kwabi table.
 */
void *
canary_alloc(size_t n)
{
    if (kwabi_api == NULL || kwabi_api->palloc == NULL)
        return NULL;

    char *p = (char *) kwabi_api->palloc(n);
    if (p != NULL) {
        allocs++;
        if (n >= 4) {
            p[0] = 'k';
            p[1] = 'w';
            p[2] = 'a';
            p[3] = 'b';
        }
    }
    return p;
}

/* Release a buffer through the ABI. */
void
canary_free(void *p)
{
    if (kwabi_api == NULL || kwabi_api->pfree == NULL)
        return;
    frees++;
    kwabi_api->pfree(p);
}

/* Report the extension's own counters, to distinguish "call arrived" from
 * "call was swallowed by the runtime". */
int
canary_alloc_count(void)
{
    return allocs;
}

int
canary_free_count(void)
{
    return frees;
}

/*
 * Raise an error through the ABI.
 *
 * Note the direction. The extension does NOT call PostgreSQL's ereport, which
 * is a variadic macro that longjmps; it formats the message itself and hands
 * the finished string to the runtime. That is the error firewall: no
 * longjmp ever unwinds through this frame.
 */
void
canary_raise_bad_handle(void)
{
    if (kwabi_api == NULL || kwabi_api->ereport == NULL)
        return;
    kwabi_api->ereport(0, "canary: bad handle", NULL);
}

/* Which ABI version we loaded against. */
unsigned int
canary_abi_version(void)
{
    return kwabi_api == NULL ? 0 : kwabi_api->version;
}
