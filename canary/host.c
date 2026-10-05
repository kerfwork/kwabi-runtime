/*
 * host.c — proof of operation for the kwabi handoff.
 *
 * Plays the part PostgreSQL will play: it supplies a native function table,
 * initialises the runtime, loads a prebuilt extension, and drives the
 * extension through the stable ABI.
 *
 * What this proves, and what it does not:
 *
 *   PROVES  an extension object with zero undefined symbols (see `nm -u
 *           libcanary.dylib`) reaches allocator and error services that live
 *           in a different shared object, through a table it was handed.
 *   PROVES  the extension was built from kwabi.h alone — no postgres.h, no
 *           PG_VERSION_NUM branches, no link against PostgreSQL.
 *   DOES NOT prove the runtime's per-version bindings are correct, because
 *   the native table here is a stub. That is what the version matrix and the
 *   canary suite over real PostgreSQL 16/17/18 will test.
 *
 * Build:
 *   cc -Wall -Wextra -I../.. -o host host.c -L../target/release \
 *      -lkwabi_runtime -Wl,-rpath,../target/release
 */

#include "kwabi.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirror of the runtime's native table. The runtime owns this struct's
 * definition; the host fills one in the way _PG_init will. */
typedef struct KwabiNative {
    uint32_t pg_major;
    void *(*palloc)(size_t);
    void *(*palloc0)(size_t);
    void *(*repalloc)(void *, size_t);
    void (*pfree)(void *);
    void *(*memory_context_current)(void);
    void *(*memory_context_switch_to)(void *);
    const char *(*error_message)(void);
    int (*error_code)(void);
    void (*error_clear)(void);
    void (*log_line)(int, const char *);
} KwabiNative;

extern const KwabiV1 *kwabi_runtime_init(const KwabiNative *native);
extern const KwabiV1 *kwabi_get_api(void);
extern int kwabi_raise(int code, const char *msg);

/* ---- stub PostgreSQL ------------------------------------------------- */

static int native_allocs = 0;
static int native_frees = 0;
static int native_logs = 0;

static void *stub_palloc(size_t n) { native_allocs++; return malloc(n ? n : 1); }
static void *stub_palloc0(size_t n) { native_allocs++; return calloc(1, n ? n : 1); }
static void *stub_repalloc(void *p, size_t n) { return realloc(p, n); }
static void stub_pfree(void *p) { native_frees++; free(p); }
static void *stub_ctx_current(void) { return (void *) 0x1234; }
static void *stub_ctx_switch_to(void *c) { return c; }
static const char *stub_error_message(void) { return "stub: postgres error"; }
static int stub_error_code(void) { return 7; }
static void stub_error_clear(void) { }
static void stub_log_line(int level, const char *msg)
{
    native_logs++;
    printf("      [stub postgres log %d] %s\n", level, msg ? msg : "(null)");
}

/* ---- checks ---------------------------------------------------------- */

static int failures = 0;

static void
check(int cond, const char *what)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

int
main(int argc, char **argv)
{
    const char *canary_path = argc > 1 ? argv[1] : "./libcanary.dylib";

    printf("kwabi handoff proof\n");
    printf("===================\n\n");

    /* 1. The host supplies a native table, as _PG_init will. */
    KwabiNative native = {
        .pg_major = 18,
        .palloc = stub_palloc,
        .palloc0 = stub_palloc0,
        .repalloc = stub_repalloc,
        .pfree = stub_pfree,
        .memory_context_current = stub_ctx_current,
        .memory_context_switch_to = stub_ctx_switch_to,
        .error_message = stub_error_message,
        .error_code = stub_error_code,
        .error_clear = stub_error_clear,
        .log_line = stub_log_line,
    };

    printf("[1] runtime init\n");
    const KwabiV1 *api = kwabi_runtime_init(&native);
    check(api != NULL, "runtime published a table");
    if (api == NULL)
        return 1;
    check(api->version == KWABI_VERSION, "table reports ABI version 1");
    check(kwabi_get_api() == api, "re-fetch returns the same address");

    printf("\n[2] load the prebuilt extension\n");
    void *h = dlopen(canary_path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        printf("  FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    check(1, "dlopen succeeded");

    bool (*ext_init)(const KwabiV1 *) = (bool (*)(const KwabiV1 *)) dlsym(h, "kwabi_ext_init");
    void *(*ext_alloc)(size_t) = (void *(*)(size_t)) dlsym(h, "canary_alloc");
    void (*ext_free)(void *) = (void (*)(void *)) dlsym(h, "canary_free");
    int (*ext_allocs)(void) = (int (*)(void)) dlsym(h, "canary_alloc_count");
    int (*ext_frees)(void) = (int (*)(void)) dlsym(h, "canary_free_count");
    unsigned (*ext_ver)(void) = (unsigned (*)(void)) dlsym(h, "canary_abi_version");
    check(ext_init && ext_alloc && ext_free && ext_allocs && ext_frees && ext_ver,
          "extension exports resolved");

    printf("\n[3] hand the table to the extension\n");
    check(ext_init(api), "kwabi_ext_init accepted the table");
    check(ext_ver() == 1, "extension reads ABI version 1 from the table");

    printf("\n[4] drive the extension through the ABI\n");
    int before = native_allocs;
    char *p = (char *) ext_alloc(64);
    check(p != NULL, "canary_alloc returned memory");
    check(native_allocs == before + 1, "the call reached the host's allocator");
    check(p && p[0] == 'k' && p[1] == 'w' && p[2] == 'a' && p[3] == 'b',
          "extension wrote its marker into that memory");
    check(ext_allocs() == 1, "extension counted the call");

    int frees_before = native_frees;
    ext_free(p);
    check(native_frees == frees_before + 1, "canary_free reached the host's free");
    check(ext_frees() == 1, "extension counted the free");

    printf("\n[5] unimplemented slots are null, not garbage\n");
    check(api->palloc != NULL, "palloc is wired");
    check(api->fmgr_info == NULL, "fmgr_info is null (not yet implemented)");
    check(api->spi_execute == NULL, "spi_execute is null (not yet implemented)");
    check(api->table_am_get == NULL, "table_am_get is null (not yet implemented)");

    printf("\n[6] error firewall\n");
    const char *msg = api->error_message ? api->error_message() : NULL;
    check(msg && strcmp(msg, "stub: postgres error") == 0,
          "with no runtime error, error_message defers to PostgreSQL");
    check(api->error_code && api->error_code() == 7, "error_code defers to PostgreSQL");

    /* A runtime-raised error must win, and must not have gone through
     * PostgreSQL's error machinery. */
    check(kwabi_raise(42, "kwabi: bad handle"), "runtime raised an error");
    check(api->error_code && api->error_code() == 42, "runtime error code wins");
    msg = api->error_message ? api->error_message() : NULL;
    check(msg && strcmp(msg, "kwabi: bad handle") == 0, "runtime message wins");

    if (api->error_clear)
        api->error_clear();
    check(api->error_code && api->error_code() == 7, "clear restores passthrough");

    printf("\n[7] ereport is transported, not longjmp'd\n");
    int logs_before = native_logs;
    api->ereport(0, "canary: bad handle", NULL);
    check(native_logs == logs_before + 1, "ereport reached the host's log sink");

    dlclose(h);

    printf("\n===================\n");
    if (failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", failures);
    return 1;
}
