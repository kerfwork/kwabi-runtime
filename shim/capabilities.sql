-- ========================================================================
-- capabilities.sql — the capability bitset must be HONEST.
--
-- A capability bit that lies is worse than no bit at all: an extension will
-- rely on it and fail in a way it cannot diagnose. So this test does three
-- things, and the third is the one that matters.
--
--   1. decode the bitset, so a failure names the bit;
--   2. assert every claimed bit is TRUE, by exercising the behaviour it
--      promises rather than by reading the bit back;
--   3. assert the bits NOT claimed are genuinely absent, and that reserved
--      bits are zero -- so the test cannot pass against a runtime that simply
--      returns all-ones.
-- ========================================================================

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_capabilities();
DROP FUNCTION IF EXISTS kwabi_cap_names();
CREATE FUNCTION kwabi_capabilities()
  RETURNS bigint AS :'bundle','kwabi_capabilities' LANGUAGE C;
CREATE FUNCTION kwabi_cap_names()
  RETURNS text AS :'bundle','kwabi_cap_names' LANGUAGE C;

\echo '=== 1. what this runtime claims ==='
SELECT kwabi_cap_names() AS claimed;

\echo ''
DROP TABLE IF EXISTS kwabi_t;
CREATE TABLE kwabi_t (tag int);

\echo '=== 2. each claimed bit must be TRUE, proven behaviourally ==='
\echo '   (not by reading the bit back -- that would be circular)'

-- CORE: memory through the ABI is genuine PostgreSQL memory.
SELECT kwabi_proof() LIKE '%genuine PostgreSQL memory%' AS core_true;

-- STRUCTURED_ERRORS: the extended fields actually arrive.
SELECT (kwabi_try_symbol(
          :'canary',
          'guarded_reports_structured__kwabi_body', 1) LIKE '%detail="%"%')
       AS structured_errors_true;

-- ERROR_FIREWALL: a failing body returns a status instead of unwinding.
SELECT (kwabi_try_symbol(
          :'canary',
          'guarded_reports_failure__kwabi_body', 1) LIKE '%status=1%')
       AS error_firewall_true;

-- ATOMIC_BODY: a body that writes then fails leaves NOTHING behind.
-- This is the bit with the strongest promise, so it gets the strongest check:
-- the write is committed only if the rollback failed to happen.
SELECT kwabi_try_write_then_fail(1) LIKE '%survivors=0%' AS atomic_body_true;

-- MEMORY_INTROSPECTION: a palloc'd pointer's chunk context matches the
-- backend's current context. A malloc'd pointer cannot pass.
SELECT kwabi_proof() LIKE '%genuine PostgreSQL memory%'
       AS memory_introspection_true;

\echo ''
\echo '=== 3. bits NOT claimed must be genuinely absent ==='
\echo '   (a runtime returning all-ones passes section 2 and FAILS here)'

-- Only six bits are defined. Nothing at or above bit 6 may be set: a runtime
-- that sets one is claiming a capability this header does not define, and an
-- extension cannot reason about it.
SELECT ((kwabi_capabilities() & ~63::bigint) = 0) AS no_undefined_bits;

-- ATOMIC_BODY is the bit most likely to be over-claimed, because it is an
-- empirical promise rather than a structural one. It must be absent on a
-- PostgreSQL major the CI matrix has not measured. On 16/17/18 it IS measured,
-- so here we assert the converse: the bit is present AND the rollback is real.
-- (The withheld case is covered in the harness against a synthetic table,
-- since a running server cannot pretend to be an unmeasured major.)
SELECT ((kwabi_capabilities() & 16) <> 0) = (kwabi_try_write_then_fail(1)
                                             LIKE '%survivors=0%')
       AS atomic_bit_matches_reality;

-- SLRU: the bit must track the load model. This script runs against a server
-- the harness started WITHOUT preload (the ordinary $(PGPORT) cluster), so the
-- bit must be CLEAR here even though the slots are wired. That is the whole
-- point of the bit: a slot test cannot tell the two cases apart.
SELECT ((kwabi_capabilities() & 32) = 0) AS slru_bit_clear_without_preload;

\echo ''
\echo '=== 4. the bootstrap rule ==='
\echo '   NULL capabilities slot means "fall back to slot tests", NOT'
\echo '   "nothing works". An older runtime must not be refused.'
\echo '   Verified in the harness against a synthetic table; a running server'
\echo '   always has the slot.'
