/* group_fmgr.c — fmgr slots for the kwabi shim */

#include "shim_internal.h"

/* Capture the error currently being handled into the runtime's error buffer.
 *
 * MUST be called from inside a PG_CATCH block: CopyErrorData() reads the error
 * stack, which only exists there. On return the error state has been flushed,
 * so the caller may continue normally — which is the whole point of the
 * firewall.
 *
 * The fields are copied into KwabiError's fixed-size arrays immediately, so the
 * palloc'd ErrorData's own memory context — which a rollback is about to tear
 * down — is never depended on. That is what makes this safe to call before
 * RollbackAndReleaseCurrentSubTransaction, and it is why the "switch context
 * first" rule (error-firewall-design.md §3.2) is satisfied by the caller
 * switching to a context that outlives the subtransaction.
 */
void
shim_capture_error(void)
{
    KwabiError err;
    ErrorData  *edata;

    kwabi_error_init(&err);

    edata = CopyErrorData();
    FlushErrorState();

    kwabi_error_set_core(&err, edata->sqlerrcode, KWABI_ERR_RAISED,
                         edata->message != NULL ? edata->message
                         : "kwabi: PostgreSQL raised without a message");
    kwabi_error_set_detail(&err, edata->detail, edata->hint);
    kwabi_error_set_object(&err,
                           edata->schema_name, edata->table_name,
                           edata->column_name, edata->datatype_name,
                           edata->constraint_name);

    FreeErrorData(edata);

    /* The runtime owns the buffer error_get reads; hand it over. */
    kwabi_error_set(&err);
}

/*
 * fmgr_info through the ABI.
 *
 * Shim-owned because fmgr_info can ereport — fmgr_info_cxt_security does
 * elog(ERROR, "cache lookup failed for function %u") — and a Rust frame between
 * that raise and a PG_TRY would break the firewall. See kwabi.h.
 *
 * NO subtransaction here, unlike the call slots below, and the distinction is
 * principled rather than an omission: fmgr_info does no user-visible work. It
 * reads catalogs and fills a struct; if it fails there is nothing to undo, so
 * the error is captured and NULL is returned. The subtransaction rule exists to
 * stop a SWALLOWED error from leaving partial WORK committed, and there is no
 * work here to leave.
 */
static KwabiFmgrInfo
shim_fmgr_info(Oid fn_oid)
{
    FmgrInfo      *flinfo = (FmgrInfo *) palloc(sizeof(FmgrInfo));
    KwabiFmgrInfo  result = NULL;

    PG_TRY();
    {
        fmgr_info(fn_oid, flinfo);
        result = (KwabiFmgrInfo) flinfo;
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = NULL;
    }
    PG_END_TRY();

    return result;
}

/*
 * The shared body of every call_function* slot.
 *
 * The subtransaction is MANDATORY, for the same reason try_body needs one and
 * measured in error-firewall-design.md §3.1: this slot catches a PostgreSQL
 * ERROR and RETURNS to the caller (the extension), which then continues.
 * Without a subtransaction, any partial work the failed function did before
 * erroring survives and can commit — the silent-inconsistency bug the firewall
 * exists to prevent. So the call runs in an internal subtransaction: success
 * commits it, failure rolls it back.
 *
 * Cost: a savepoint per call. That is the price of catching safely, and it is
 * the same price try_body pays. A cheaper read-only fast path is an open
 * question (error-firewall-design.md §8.3) and is deliberately not taken here,
 * because "is this call safe to run without undo?" is not a question the ABI
 * can answer for an arbitrary function.
 *
 * FunctionCallInvoke does NOT short-circuit a strict function, so a NULL
 * argument is passed through to the function; strictness is a caller-side
 * concern. See the contract in kwabi.h.
 */
static KwabiStatus
shim_call_impl(FmgrInfo *flinfo, int nargs, const Datum *args,
               const bool *argnulls, bool *isnull, Datum *result)
{
    KwabiStatus    status;
    MemoryContext  oldcontext = CurrentMemoryContext;
    ResourceOwner  oldowner   = CurrentResourceOwner;

    BeginInternalSubTransaction(NULL);

    PG_TRY();
    {
        LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
        int i;

        InitFunctionCallInfoData(*fcinfo, flinfo, nargs, InvalidOid, NULL, NULL);
        for (i = 0; i < nargs; i++)
        {
            fcinfo->args[i].value  = args[i];
            fcinfo->args[i].isnull = (argnulls != NULL) ? argnulls[i] : false;
        }

        *result = FunctionCallInvoke(fcinfo);
        *isnull = fcinfo->isnull;

        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;
        status = KWABI_OK;
    }
    PG_CATCH();
    {
        /* Switch to a context that outlives the subtransaction BEFORE
         * CopyErrorData pallocs into it — the documented order (§3.2). */
        MemoryContextSwitchTo(oldcontext);
        shim_capture_error();

        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;
        status = KWABI_ERR_RAISED;
    }
    PG_END_TRY();

    return status;
}

static KwabiStatus
shim_call_function(KwabiFmgrInfo info, int nargs, Datum *args,
                   const bool *argnulls, bool *isnull, Datum *result)
{
    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;
    if (nargs < 0 || nargs > FUNC_MAX_ARGS)
        return KWABI_ERR_BAD_ARG;
    if (nargs > 0 && args == NULL)
        return KWABI_ERR_BAD_ARG;

    return shim_call_impl((FmgrInfo *) info, nargs, args, argnulls, isnull, result);
}

static KwabiStatus
shim_call_function1(KwabiFmgrInfo info, Datum arg1, bool *isnull, Datum *result)
{
    Datum a[1];

    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;

    a[0] = arg1;
    return shim_call_impl((FmgrInfo *) info, 1, a, NULL, isnull, result);
}

static KwabiStatus
shim_call_function2(KwabiFmgrInfo info, Datum arg1, Datum arg2,
                    bool *isnull, Datum *result)
{
    Datum a[2];

    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;

    a[0] = arg1;
    a[1] = arg2;
    return shim_call_impl((FmgrInfo *) info, 2, a, NULL, isnull, result);
}

static KwabiStatus
shim_call_function3(KwabiFmgrInfo info, Datum arg1, Datum arg2, Datum arg3,
                    bool *isnull, Datum *result)
{
    Datum a[3];

    if (info == NULL || isnull == NULL || result == NULL)
        return KWABI_ERR_BAD_ARG;

    a[0] = arg1;
    a[1] = arg2;
    a[2] = arg3;
    return shim_call_impl((FmgrInfo *) info, 3, a, NULL, isnull, result);
}

void
init_group_fmgr(void)
{
    shim_table.fmgr_info = shim_fmgr_info;
    shim_table.call_function = shim_call_function;
    shim_table.call_function1 = shim_call_function1;
    shim_table.call_function2 = shim_call_function2;
    shim_table.call_function3 = shim_call_function3;
}
