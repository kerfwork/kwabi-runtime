-- ========================================================================
-- capability-consumer.sql — the bitset CONSUMED, by an extension.
--
-- capabilities.sql proves the bitset is honest: every claimed bit is proven
-- behaviourally, and no undefined bit is set. That is a fact about the shim.
--
-- This file proves the other half, which was missing until now: that an
-- extension can *act* on it. The bitset existed and was true, but nothing
-- branched on it, so "does a bit actually control behaviour?" was untested at
-- the exact point extensions would ask the question.
--
-- ORDER MATTERS HERE, and deliberately. The first check runs a body through
-- kwabi_try_symbol before anything else has touched the extension, because
-- that is the only moment at which "does kwabi_try_symbol hand the extension
-- its table?" is a real question. A psql session is one backend, so a global
-- set by an earlier statement stays set; putting the names check first would
-- have initialised the extension and made every later check pass regardless.
-- That was tried, and the negative control caught it: skipping init in
-- kwabi_try_symbol still passed, because the earlier statement had done the
-- work. An implicit dependency on statement order is a race, not a test.
-- ========================================================================

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_capabilities();
DROP FUNCTION IF EXISTS kwabi_cap_names();
DROP FUNCTION IF EXISTS kwabi_try_symbol(text, text, int4);
DROP FUNCTION IF EXISTS kwabi_ext_capability_names(text);

CREATE FUNCTION kwabi_capabilities()
  RETURNS bigint AS :'bundle','kwabi_capabilities' LANGUAGE C;
CREATE FUNCTION kwabi_cap_names()
  RETURNS text AS :'bundle','kwabi_cap_names' LANGUAGE C;
CREATE FUNCTION kwabi_try_symbol(text, text, int4)
  RETURNS text AS :'bundle','kwabi_try_symbol' LANGUAGE C;
CREATE FUNCTION kwabi_ext_capability_names(text)
  RETURNS text AS :'bundle','kwabi_ext_capability_names' LANGUAGE C;

\echo '=== 1. a body can reach the runtime, through kwabi_try_symbol ==='
\echo '   THE first statement to touch the extension, so this is the only'
\echo '   real test that try_symbol hands over the table. Before this change'
\echo '   the SDK stored nothing at init here, so a body asking the SDK'
\echo '   anything got None.'
\echo ''
\echo '   required=16 (ATOMIC_BODY), which this runtime guarantees -> runs.'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 16)
        LIKE '%status=0%') AS body_reached_the_runtime;

\echo ''
\echo '--- and it must NOT be the "not initialised" path ---'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 16)
        NOT LIKE '%not initialised%') AS table_was_handed_over;

\echo ''
\echo '=== 2. the extension reaches the bitset through the SDK ==='
\echo '   (before this change the SDK table stopped 6 slots short of'
\echo '    kwabi.h, so api->capabilities was not reachable at all)'
SELECT kwabi_ext_capability_names(:'libdir' || '/' || :'guarded') AS from_extension;

\echo ''
\echo '=== 3. the shim and the extension must agree ==='
\echo '   Two different paths to the same answer:'
\echo '     shim      -> the published table -> capabilities_of'
\echo '     extension -> init stored pointer -> SDK mirror -> bootstrap rule'
\echo '   A disagreement means the SDK mirror has drifted from the header,'
\echo '   while every shim-side test stays green.'
SELECT kwabi_ext_capability_names(:'libdir' || '/' || :'guarded')
       = kwabi_cap_names() AS paths_agree;

\echo ''
\echo '=== 4. THE point: an unguaranteed bit is refused, not assumed ==='
\echo '   required=32 is bit 5 -- not defined by this header. A runtime must'
\echo '   never claim it, so the body must refuse. This is the branch an'
\echo '   extension that WRITES takes before relying on rollback: catching an'
\echo '   error without a subtransaction leaves partial work committed, which'
\echo '   is worse than not catching it.'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 32)
        LIKE '%status=1%') AS refused_when_unguaranteed;

\echo ''
\echo '--- the refusal must NAME the missing bit, not just fail ---'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 32)
        LIKE '%missing=0x20%') AS names_missing_bit;

\echo ''
\echo '--- and it must report the runtime it actually asked ---'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 32)
        LIKE '%ATOMIC_BODY%') AS reports_what_runtime_claims;

\echo ''
\echo '=== 5. a mask of several bits is checked as a whole ==='
\echo '   required=7 (CORE|STRUCTURED_ERRORS|ERROR_FIREWALL) is all present.'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 7)
        LIKE '%status=0%') AS mask_present;

\echo ''
\echo '--- required=23 (adds MEMORY_INTROSPECTION + ATOMIC_BODY) also holds ---'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 23)
        LIKE '%status=0%') AS full_mask_present;

\echo ''
\echo '--- a mask is only satisfied if EVERY bit is: 32|16 must be refused ---'
SELECT (kwabi_try_symbol(:'libdir' || '/' || :'guarded',
                         'guarded_requires_capability__kwabi_body', 48)
        LIKE '%status=1%') AS one_missing_bit_refuses_the_mask;

\echo ''
\echo '=== capability-consumer tests complete ==='
