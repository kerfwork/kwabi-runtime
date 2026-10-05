/*
 * errsize.c — cross-version safety for the error channel.
 *
 * The scenario this proves: an extension built against an older header passes
 * a SMALLER KwabiError than the runtime knows about. The runtime must not write
 * past the end of it.
 *
 * This is not hypothetical. "Upgrade PostgreSQL without rebuilding your
 * extension" is the product, so a v1 extension meeting a v2 runtime is the
 * normal case, not an edge case.
 *
 * Built as an ordinary PostgreSQL module so it can be driven from SQL. The
 * structs are declared here with the exact layouts from kwabi.h, because the
 * point is to simulate a caller compiled against a different header.
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/*
 * A v1 caller's error struct: 524 bytes. Copied from the shipped v1 layout.
 * Deliberately NOT including kwabi.h — this translation unit stands in for a
 * caller that only has the old header.
 */
typedef struct ErrV1 {
    uint32_t size;
    int32_t  sqlerrcode;
    int32_t  status;
    char     message[512];
} ErrV1;

/*
 * The v2 writer, as the runtime implements it: writes only fields that fit
 * inside `size`, and never past it.
 *
 * Mirrors kwabi_error_set_core / _set_detail / _set_object.
 */
static void
write_error_v2(char *base, uint32_t caller_size,
               int sqlerrcode, int status, const char *msg,
               const char *detail, const char *hint)
{
    /* Offsets from kwabi.h v2. */
    const uint32_t off_sqlerrcode = 4;
    const uint32_t off_status     = 8;
    const uint32_t off_message    = 12;
    const uint32_t off_detail     = 12 + 512;
    const uint32_t off_hint       = off_detail + 512;

    if (base == NULL || caller_size < off_message + 32)
        return;

    memcpy(base + 0, &caller_size, sizeof(uint32_t));  /* echo back */
    memcpy(base + off_sqlerrcode, &sqlerrcode, sizeof(int32_t));
    memcpy(base + off_status, &status, sizeof(int32_t));
    strlcpy(base + off_message, msg, 512);

    /* Only write these if the caller's struct actually extends this far. */
    if (caller_size >= off_detail + 32)
        strlcpy(base + off_detail, detail, 512);
    if (caller_size >= off_hint + 32)
        strlcpy(base + off_hint, hint, 384);
}

/*
 * err_size_v1() / err_size_v2() — report the struct sizes so the test can
 * assert they differ and that the shared prefix is identical.
 */
PG_FUNCTION_INFO_V1(err_size_v1);
PG_FUNCTION_INFO_V1(err_size_v2);

Datum
err_size_v1(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT32((int32) sizeof(ErrV1));
}

Datum
err_size_v2(PG_FUNCTION_ARGS)
{
    /* sizeof(kwabi.h v2 KwabiError), computed from its field sizes. */
    PG_RETURN_INT32((int32) (12 + 512 + 512 + 384 + 5 * 128));
}

/*
 * err_write_v1_caller() — simulate a v1 caller, with a guard band.
 *
 * Allocates a v1-sized buffer PLUS a canary region after it, fills the canary
 * with a known pattern, runs the v2 writer against the v1 size, then checks the
 * canary is intact. If the writer overran, the canary is corrupted and the test
 * fails loudly rather than silently corrupting adjacent memory.
 */
PG_FUNCTION_INFO_V1(err_write_v1_caller);

#define CANARY_BYTES 64
#define CANARY_PATTERN 0xA5

Datum
err_write_v1_caller(PG_FUNCTION_ARGS)
{
    uint32_t v1_size = (uint32_t) sizeof(ErrV1);
    uint32_t total = v1_size + CANARY_BYTES;

    char *buf = (char *) palloc(total);
    memset(buf, 0, total);
    memset(buf + v1_size, CANARY_PATTERN, CANARY_BYTES);

    /* The writer believes it is writing a v2 struct, but must respect size. */
    write_error_v2(buf, v1_size,
                   0x50000, 1,
                   "kwabi: extension reported a failure",
                   "detail that must not be written past the caller's buffer",
                   "hint that must not be written either");

    /* Was the guard band touched? */
    int overrun = 0;
    for (int i = 0; i < CANARY_BYTES; i++) {
        if ((unsigned char) buf[v1_size + i] != CANARY_PATTERN) {
            overrun++;
        }
    }

    /* And did the caller get usable error information? */
    ErrV1 *e = (ErrV1 *) buf;
    char msg[64];
    strlcpy(msg, e->message, sizeof(msg));

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("v1 caller: size_field=%u status=%d sqlerrcode=%d "
                 "overrun_bytes=%d msg=\"%s\"",
                 e->size, e->status, e->sqlerrcode, overrun, msg)));
}

/*
 * err_write_v2_caller() — the same writer against a full v2 buffer, to show the
 * optional fields DO get written when there is room.
 */
PG_FUNCTION_INFO_V1(err_write_v2_caller);

Datum
err_write_v2_caller(PG_FUNCTION_ARGS)
{
    uint32_t v2_size = (uint32_t) (12 + 512 + 512 + 384 + 5 * 128);
    uint32_t total = v2_size + CANARY_BYTES;

    char *buf = (char *) palloc(total);
    memset(buf, 0, total);
    memset(buf + v2_size, CANARY_PATTERN, CANARY_BYTES);

    write_error_v2(buf, v2_size,
                   0x50000, 1,
                   "kwabi: extension reported a failure",
                   "the detail line",
                   "the hint line");

    int overrun = 0;
    for (int i = 0; i < CANARY_BYTES; i++) {
        if ((unsigned char) buf[v2_size + i] != CANARY_PATTERN)
            overrun++;
    }

    char detail[64], hint[64];
    strlcpy(detail, buf + 12 + 512, sizeof(detail));
    strlcpy(hint, buf + 12 + 512 + 512, sizeof(hint));

    PG_RETURN_TEXT_P(cstring_to_text(
        psprintf("v2 caller: overrun_bytes=%d detail=\"%s\" hint=\"%s\"",
                 overrun, detail, hint)));
}
