/*
 * kwabi_sdk.h — kwabi C SDK
 *
 * This header provides a C wrapper around the raw kwabi function table.
 * It offers inline helper functions, error handling macros, and convenience
 * functions for common patterns.
 *
 * Extensions include this header instead of kwabi.h directly.
 */

#ifndef KWABI_SDK_H
#define KWABI_SDK_H

#include "kwabi.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Error handling macros
 * ======================================================================== */

#define KWABI_TRY(api, expr) \
    do { \
        (api)->error_clear(); \
        (expr); \
        if ((api)->error_code() != 0) { \
            return (api)->error_code(); \
        } \
    } while (0)

#define KWABI_TRY_NULL(api, expr) \
    do { \
        (api)->error_clear(); \
        void *_result = (expr); \
        if ((api)->error_code() != 0) { \
            return NULL; \
        } \
        return _result; \
    } while (0)

#define KWABI_THROW(api, errcode, ...) \
    do { \
        (api)->ereport((errcode), __VA_ARGS__); \
        return (errcode); \
    } while (0)

/* ========================================================================
 * Memory helpers
 * ======================================================================== */

static inline void *kwabi_alloc(const KwabiV1 *api, size_t size) {
    return api->palloc(size);
}

static inline void *kwabi_alloc0(const KwabiV1 *api, size_t size) {
    return api->palloc0(size);
}

static inline void *kwabi_realloc(const KwabiV1 *api, void *ptr, size_t size) {
    return api->repalloc(ptr, size);
}

static inline void kwabi_free(const KwabiV1 *api, void *ptr) {
    api->pfree(ptr);
}

/* ========================================================================
 * String helpers
 * ======================================================================== */

static inline char *kwabi_strdup(const KwabiV1 *api, const char *str) {
    size_t len = strlen(str) + 1;
    char *copy = (char *)api->palloc(len);
    memcpy(copy, str, len);
    return copy;
}

static inline char *kwabi_strndup(const KwabiV1 *api, const char *str, size_t n) {
    char *copy = (char *)api->palloc(n + 1);
    memcpy(copy, str, n);
    copy[n] = '\0';
    return copy;
}

/* ========================================================================
 * SPI helpers
 * ======================================================================== */

static inline KwabiSPIResult *kwabi_spi_query(const KwabiV1 *api, const char *sql) {
    return api->spi_execute(sql, false, 0);
}

static inline KwabiSPIResult *kwabi_spi_query_read_only(const KwabiV1 *api, const char *sql) {
    return api->spi_execute(sql, true, 0);
}

static inline Datum kwabi_spi_get_datum(const KwabiV1 *api, KwabiSPIResult *result, int tupno, int attno) {
    return api->spi_result_get_value(result, tupno, attno);
}

static inline void kwabi_spi_done(const KwabiV1 *api, KwabiSPIResult *result) {
    api->spi_free_result(result);
}

/* ========================================================================
 * Relation helpers
 * ======================================================================== */

static inline KwabiRelation kwabi_rel_open(const KwabiV1 *api, Oid relid) {
    return api->relation_open(relid, KWABI_LOCKMODE_SHARE);
}

static inline KwabiRelation kwabi_rel_open_exclusive(const KwabiV1 *api, Oid relid) {
    return api->relation_open(relid, KWABI_LOCKMODE_EXCLUSIVE);
}

static inline void kwabi_rel_close(const KwabiV1 *api, KwabiRelation rel) {
    api->relation_close(rel, KWABI_LOCKMODE_SHARE);
}

static inline int kwabi_rel_natts(const KwabiV1 *api, KwabiRelation rel) {
    return api->tuple_natts(api->relation_tupledesc(rel));
}

static inline const char *kwabi_rel_name(const KwabiV1 *api, KwabiRelation rel) {
    return api->relation_name(rel);
}

static inline Oid kwabi_rel_oid(const KwabiV1 *api, KwabiRelation rel) {
    return api->relation_id(rel);
}

/* ========================================================================
 * Tuple helpers
 * ======================================================================== */

static inline int kwabi_tupdesc_natts(const KwabiV1 *api, TupleDesc tupdesc) {
    return api->tuple_natts(tupdesc);
}

static inline Oid kwabi_tupdesc_typeid(const KwabiV1 *api, TupleDesc tupdesc, int attno) {
    return api->tuple_typeid(tupdesc, attno);
}

static inline const char *kwabi_tupdesc_attname(const KwabiV1 *api, TupleDesc tupdesc, int attno) {
    return api->tuple_attname(tupdesc, attno);
}

static inline int kwabi_tupdesc_attnum(const KwabiV1 *api, TupleDesc tupdesc, const char *attname) {
    return api->tuple_attnum(tupdesc, attname);
}

static inline bool kwabi_tupdesc_attisdropped(const KwabiV1 *api, TupleDesc tupdesc, int attno) {
    return api->tuple_attisdropped(tupdesc, attno);
}

/* ========================================================================
 * Node helpers
 * ======================================================================== */

static inline KwabiNodeType kwabi_node_type(const KwabiV1 *api, KwabiNode *node) {
    return api->node_type(node);
}

static inline const char *kwabi_node_type_name(const KwabiV1 *api, KwabiNode *node) {
    return api->node_type_name(node);
}

static inline int kwabi_node_list_length(const KwabiV1 *api, KwabiNode *node) {
    return api->node_list_length(node);
}

static inline KwabiNode *kwabi_node_list_get(const KwabiV1 *api, KwabiNode *node, int index) {
    return api->node_list_get(node, index);
}

static inline void kwabi_node_free(const KwabiV1 *api, KwabiNode *node) {
    api->free_node(node);
}

/* ========================================================================
 * Query helpers
 * ======================================================================== */

static inline KwabiCmdType kwabi_query_command_type(const KwabiV1 *api, KwabiNode *query) {
    return api->query_command_type(query);
}

static inline List *kwabi_query_target_list(const KwabiV1 *api, KwabiNode *query) {
    return api->query_target_list(query);
}

static inline List *kwabi_query_returning_list(const KwabiV1 *api, KwabiNode *query) {
    return api->query_returning_list(query);
}

static inline bool kwabi_query_has_for_update(const KwabiV1 *api, KwabiNode *query) {
    return api->query_has_for_update(query);
}

/* ========================================================================
 * Transaction helpers
 * ======================================================================== */

static inline void kwabi_begin(const KwabiV1 *api) {
    api->transaction_start();
}

static inline void kwabi_commit(const KwabiV1 *api) {
    api->transaction_commit();
}

static inline void kwabi_abort(const KwabiV1 *api) {
    api->transaction_abort();
}

static inline bool kwabi_in_transaction(const KwabiV1 *api) {
    return api->transaction_is_active();
}

/* ========================================================================
 * GUC helpers
 * ======================================================================== */

static inline int kwabi_guc_int(const KwabiV1 *api, const char *name) {
    return api->guc_get_int(name);
}

static inline const char *kwabi_guc_string(const KwabiV1 *api, const char *name) {
    return api->guc_get_string(name);
}

static inline bool kwabi_guc_bool(const KwabiV1 *api, const char *name) {
    return api->guc_get_bool(name);
}

static inline double kwabi_guc_float(const KwabiV1 *api, const char *name) {
    return api->guc_get_float(name);
}

/* ========================================================================
 * Extension helpers
 * ======================================================================== */

static inline bool kwabi_extension_exists(const KwabiV1 *api, const char *extname) {
    return api->extension_installed(extname);
}

static inline Oid kwabi_extension_oid(const KwabiV1 *api, const char *extname) {
    return api->extension_oid(extname);
}

static inline const char *kwabi_extension_version(const KwabiV1 *api, const char *extname) {
    return api->extension_version(extname);
}

/* ========================================================================
 * Sequence helpers
 * ======================================================================== */

static inline int64 kwabi_sequence_nextval(const KwabiV1 *api, Oid seq_oid) {
    return api->sequence_nextval(seq_oid);
}

static inline int64 kwabi_sequence_currval(const KwabiV1 *api, Oid seq_oid) {
    return api->sequence_currval(seq_oid);
}

static inline int64 kwabi_sequence_setval(const KwabiV1 *api, Oid seq_oid, int64 value) {
    return api->sequence_setval(seq_oid, value);
}

/* ========================================================================
 * Background worker helpers
 * ======================================================================== */

static inline Oid kwabi_bgworker_register(const KwabiV1 *api, const char *name, bgworker_main_type main, void *arg) {
    return api->bgworker_register(name, main, arg);
}

static inline void kwabi_bgworker_terminate(const KwabiV1 *api, Oid bgw_oid) {
    api->bgworker_terminate(bgw_oid);
}

static inline bool kwabi_bgworker_is_running(const KwabiV1 *api, Oid bgw_oid) {
    return api->bgworker_is_running(bgw_oid);
}

/* ========================================================================
 * Lock helpers
 * ======================================================================== */

static inline void kwabi_lock_acquire(const KwabiV1 *api, LWLock *lock, KwabiLWLockMode mode) {
    api->lock_acquire(lock, mode);
}

static inline void kwabi_lock_release(const KwabiV1 *api, LWLock *lock) {
    api->lock_release(lock);
}

static inline bool kwabi_lock_held_by_me(const KwabiV1 *api, LWLock *lock) {
    return api->lock_held_by_me(lock);
}

static inline void kwabi_spin_acquire(const KwabiV1 *api, slock_t *lock) {
    api->spin_acquire(lock);
}

static inline void kwabi_spin_release(const KwabiV1 *api, slock_t *lock) {
    api->spin_release(lock);
}

/* ========================================================================
 * Shared memory helpers
 * ======================================================================== */

static inline void *kwabi_shmem_alloc(const KwabiV1 *api, size_t size) {
    return api->shmem_alloc(size);
}

static inline void kwabi_shmem_free(const KwabiV1 *api, void *ptr) {
    api->shmem_free(ptr);
}

static inline void *kwabi_shmem_get(const KwabiV1 *api, const char *name, size_t size) {
    return api->shmem_get(name, size);
}

/* ========================================================================
 * StringInfo helpers
 * ======================================================================== */

static inline void kwabi_stringinfo_init(const KwabiV1 *api, StringInfo str) {
    api->stringinfo_init(str);
}

static inline void kwabi_stringinfo_reset(const KwabiV1 *api, StringInfo str) {
    api->stringinfo_reset(str);
}

static inline void kwabi_stringinfo_append(const KwabiV1 *api, StringInfo str, const char *data) {
    api->stringinfo_append(str, data);
}

static inline void kwabi_stringinfo_append_char(const KwabiV1 *api, StringInfo str, char c) {
    api->stringinfo_append_char(str, c);
}

static inline void kwabi_stringinfo_append_int(const KwabiV1 *api, StringInfo str, int64 value) {
    api->stringinfo_append_int(str, value);
}

static inline const char *kwabi_stringinfo_data(const KwabiV1 *api, StringInfo str) {
    return api->stringinfo_data(str);
}

static inline int kwabi_stringinfo_len(const KwabiV1 *api, StringInfo str) {
    return api->stringinfo_len(str);
}

/* ========================================================================
 * Type helpers
 * ======================================================================== */

static inline Datum kwabi_type_input(const KwabiV1 *api, Oid type_oid, const char *input, int32 typmod) {
    return api->type_input(type_oid, input, typmod);
}

static inline char *kwabi_type_output(const KwabiV1 *api, Oid type_oid, Datum value) {
    return api->type_output(type_oid, value);
}

static inline Datum kwabi_type_recv(const KwabiV1 *api, Oid type_oid, StringInfo buf) {
    return api->type_recv(type_oid, buf);
}

static inline void kwabi_type_send(const KwabiV1 *api, Oid type_oid, Datum value, StringInfo buf) {
    api->type_send(type_oid, value, buf);
}

static inline Oid kwabi_type_element_type(const KwabiV1 *api, Oid type_oid) {
    return api->type_element_type(type_oid);
}

static inline int16 kwabi_type_length(const KwabiV1 *api, Oid type_oid) {
    return api->type_length(type_oid);
}

static inline bool kwabi_type_is_array(const KwabiV1 *api, Oid type_oid) {
    return api->type_is_array(type_oid);
}

static inline bool kwabi_type_is_composite(const KwabiV1 *api, Oid type_oid) {
    return api->type_is_composite(type_oid);
}

/* ========================================================================
 * Parser helpers
 * ======================================================================== */

static inline KwabiNode *kwabi_parse_expr(const KwabiV1 *api, const char *sql, Oid *argtypes, int nargs) {
    return api->parse_expr(sql, argtypes, nargs);
}

static inline KwabiNode *kwabi_parse_type(const KwabiV1 *api, const char *type_name) {
    return api->parse_type(type_name);
}

static inline Oid kwabi_oper_left_type(const KwabiV1 *api, Oid oper_oid) {
    return api->oper_left_type(oper_oid);
}

static inline Oid kwabi_oper_right_type(const KwabiV1 *api, Oid oper_oid) {
    return api->oper_right_type(oper_oid);
}

static inline Oid kwabi_oper_result_type(const KwabiV1 *api, Oid oper_oid) {
    return api->oper_result_type(oper_oid);
}

/* ========================================================================
 * Replication helpers
 * ======================================================================== */

static inline KwabiLogicalDecodingCtx *kwabi_logical_decoding_begin(const KwabiV1 *api, Oid slot_oid, int64 start_lsn) {
    return api->logical_decoding_begin(slot_oid, start_lsn);
}

static inline void kwabi_logical_decoding_end(const KwabiV1 *api, KwabiLogicalDecodingCtx *ctx) {
    api->logical_decoding_end(ctx);
}

static inline bool kwabi_logical_decoding_read(const KwabiV1 *api, KwabiLogicalDecodingCtx *ctx, int64 *lsn, StringInfo *data) {
    return api->logical_decoding_read(ctx, lsn, data);
}

/* ========================================================================
 * Convenience: run a query and get a single Datum result
 * ======================================================================== */

static inline Datum kwabi_query_single_datum(const KwabiV1 *api, const char *sql, bool *is_null) {
    KwabiSPIResult *result = api->spi_execute(sql, true, 1);
    if (result == NULL) {
        *is_null = true;
        return (Datum)0;
    }
    
    int ntuples = api->spi_result_ntuples(result);
    if (ntuples == 0) {
        api->spi_free_result(result);
        *is_null = true;
        return (Datum)0;
    }
    
    Datum value = api->spi_result_get_value(result, 0, 0);
    api->spi_free_result(result);
    *is_null = false;
    return value;
}

/* ========================================================================
 * Convenience: run a query and get a single int64 result
 * ======================================================================== */

static inline int64 kwabi_query_single_int64(const KwabiV1 *api, const char *sql, bool *is_null) {
    Datum value = kwabi_query_single_datum(api, sql, is_null);
    if (*is_null) {
        return 0;
    }
    return (int64)value;
}

/* ========================================================================
 * Convenience: run a query and get a single string result
 * ======================================================================== */

static inline char *kwabi_query_single_string(const KwabiV1 *api, const char *sql, bool *is_null) {
    Datum value = kwabi_query_single_datum(api, sql, is_null);
    if (*is_null) {
        return NULL;
    }
    return (char *)value;
}

/* ========================================================================
 * Convenience: run a query and get a single Oid result
 * ======================================================================== */

static inline Oid kwabi_query_single_oid(const KwabiV1 *api, const char *sql, bool *is_null) {
    Datum value = kwabi_query_single_datum(api, sql, is_null);
    if (*is_null) {
        return 0;
    }
    return (Oid)value;
}

#ifdef __cplusplus
}
#endif

#endif /* KWABI_SDK_H */
