/*
 * uint_body.c — the uint64 canary's text I/O, as a reloadable type body.
 *
 * Built twice, with -DOUT_PREFIX='""' (v1) and -DOUT_PREFIX='"u"' (v2). The two differ only in
 * how a value is printed, so a reload is visible in the output and the stored bits are the
 * same. Input is strict decimal: digits only, no sign or space, and out-of-range is an error.
 */
#include "kwabi.h"

#include <stdio.h>
#include <string.h>

#ifndef OUT_PREFIX
#error "build with -DOUT_PREFIX='\"\"' or -DOUT_PREFIX='\"u\"'"
#endif

/* PostgreSQL's SQLSTATE encoding (utils/elog.h), reproduced so this body needs no headers. */
#define PG_SIXBIT(ch) (((ch) - '0') & 0x3F)
#define PG_SQLSTATE(a, b, c, d, e) \
    (PG_SIXBIT(a) | (PG_SIXBIT(b) << 6) | (PG_SIXBIT(c) << 12) | \
     (PG_SIXBIT(d) << 18) | (PG_SIXBIT(e) << 24))
#define SQLSTATE_INVALID_TEXT PG_SQLSTATE('2', '2', 'P', '0', '2')
#define SQLSTATE_OUT_OF_RANGE PG_SQLSTATE('2', '2', '0', '0', '3')

static KwabiStatus
uint_input(const char *text, uint64_t *value, KwabiError *err, void *arg)
{
    uint64_t    v = 0;
    const char *p = text;

    (void) arg;
    if (*p == '\0')
        goto syntax;
    for (; *p != '\0'; p++)
    {
        unsigned    d;

        if (*p < '0' || *p > '9')
            goto syntax;
        d = (unsigned) (*p - '0');
        /* v * 10 + d must fit in 64 bits. */
        if (v > (UINT64_MAX - d) / 10)
        {
            kwabi_error_init(err);
            kwabi_error_set_core(err, SQLSTATE_OUT_OF_RANGE, KWABI_ERR_BODY_RAISED,
                                 "value out of range for type uint64");
            return KWABI_ERR_BODY_RAISED;
        }
        v = v * 10 + d;
    }
    *value = v;
    return KWABI_OK;

syntax:
    kwabi_error_init(err);
    kwabi_error_set_core(err, SQLSTATE_INVALID_TEXT, KWABI_ERR_BODY_RAISED,
                         "invalid input syntax for type uint64");
    return KWABI_ERR_BODY_RAISED;
}

static KwabiStatus
uint_output(uint64_t value, char *buf, size_t buflen, KwabiError *err, void *arg)
{
    int         n;

    (void) arg;
    n = snprintf(buf, buflen, "%s%llu", OUT_PREFIX, (unsigned long long) value);
    if (n < 0 || (size_t) n >= buflen)
    {
        kwabi_error_init(err);
        kwabi_error_set_core(err, 0, KWABI_ERR_BAD_ARG, "output buffer too small");
        return KWABI_ERR_BAD_ARG;
    }
    return KWABI_OK;
}

static const KwabiTypeBodies type_bodies = {
    .size = sizeof(KwabiTypeBodies),
    .version = KWABI_TYPE_BODIES_VERSION,
    .input = uint_input,
    .output = uint_output,
};

const KwabiTypeBodies *
kwabi_type_bodies(void)
{
    return &type_bodies;
}
