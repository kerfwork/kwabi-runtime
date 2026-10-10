/*
 * agg_body.c — a bound aggregate body for aggregate-api.sql.
 *
 * Summing with a scale. Built twice: -DSCALE=1 (v1) and -DSCALE=1000 (v2), so a reload is
 * visible in the result. A value of 13 is refused by step, so an error path can be tested.
 * The state is an int64 total; combine adds totals; serialize writes the total.
 */
#include "kwabi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef SCALE
#error "build with -DSCALE=1 or -DSCALE=1000"
#endif

/* 22003 (numeric_value_out_of_range), PostgreSQL's SQLSTATE encoding. */
#define SIX(ch) (((ch) - '0') & 0x3F)
#define SQLSTATE_NUMERIC_OUT_OF_RANGE \
    (SIX('2') | (SIX('2') << 6) | (SIX('0') << 12) | (SIX('0') << 18) | (SIX('3') << 24))

typedef struct { int64_t total; } State;

static KwabiStatus agg_init(void **st, KwabiError *err, void *arg) {
    (void) err; (void) arg;
    State *s = calloc(1, sizeof *s);
    if (s == NULL) return KWABI_ERR_BAD_ARG;
    *st = s;
    return KWABI_OK;
}

static KwabiStatus agg_step(void *st, uint64_t value, KwabiError *err, void *arg) {
    (void) arg;
    if ((int64_t) value == 13) {
        kwabi_error_init(err);
        kwabi_error_set_core(err, SQLSTATE_NUMERIC_OUT_OF_RANGE,
                             KWABI_ERR_BODY_RAISED, "unlucky value 13");
        return KWABI_ERR_BODY_RAISED;
    }
    ((State *) st)->total += (int64_t) value * SCALE;
    return KWABI_OK;
}

static KwabiStatus agg_inverse(void *st, uint64_t value, KwabiError *err, void *arg) {
    (void) err; (void) arg;
    ((State *) st)->total -= (int64_t) value * SCALE;
    return KWABI_OK;
}

static KwabiStatus agg_combine(void *st, const void *other, KwabiError *err, void *arg) {
    (void) err; (void) arg;
    ((State *) st)->total += ((const State *) other)->total;
    return KWABI_OK;
}

static KwabiStatus agg_final(const void *st, uint64_t *result, KwabiError *err, void *arg) {
    (void) err; (void) arg;
    *result = (uint64_t) ((const State *) st)->total;
    return KWABI_OK;
}

static KwabiStatus agg_serialize(const void *st, char *buf, size_t buflen, size_t *used,
                                 KwabiError *err, void *arg) {
    (void) err; (void) arg;
    if (buflen < sizeof(int64_t)) return KWABI_ERR_BAD_ARG;
    memcpy(buf, st, sizeof(int64_t));
    *used = sizeof(int64_t);
    return KWABI_OK;
}

static KwabiStatus agg_deserialize(const char *buf, size_t len, void **st, KwabiError *err, void *arg) {
    (void) err; (void) arg;
    if (len != sizeof(int64_t)) return KWABI_ERR_BAD_ARG;
    State *s = calloc(1, sizeof *s);
    if (s == NULL) return KWABI_ERR_BAD_ARG;
    memcpy(&s->total, buf, sizeof(int64_t));
    *st = s;
    return KWABI_OK;
}

static const KwabiAggBodies table = {
    .size = sizeof(KwabiAggBodies),
    .version = KWABI_AGG_BODIES_VERSION,
    .init = agg_init,
    .step = agg_step,
    .final = agg_final,
    .inverse = agg_inverse,
    .combine = agg_combine,
    .serialize = agg_serialize,
    .deserialize = agg_deserialize,
    .arg = NULL,
};

const KwabiAggBodies *kwabi_aggregate_bodies(void) { return &table; }

bool kwabi_ext_init(const KwabiV1 *api) { return api != NULL; }
