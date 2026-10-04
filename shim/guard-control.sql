-- guard-control.sql — the control for guard-test.sql.
--
-- A hand-written `extern "C"` body that panics. This is what the contract
-- forbids and what #[guarded_body] exists to make unnecessary.
--
-- Run SEPARATELY: it aborts the postmaster. If this does NOT abort, the
-- guard-test result is meaningless, because it would mean panics are contained
-- anyway and the macro is not doing the work.

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '=== CONTROL: an unguarded panicking body ==='
\echo '(must abort; that is the point of the comparison)'

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_try_symbol(text, text, int4);
CREATE FUNCTION kwabi_try_symbol(text, text, int4)
    RETURNS text AS :'bundle', 'kwabi_try_symbol' LANGUAGE C;

SELECT kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                        'raw_panicking_body', 1) AS unguarded_panic;

\echo '--- not reached: the postmaster aborted ---'
SELECT 'still alive' AS after;
