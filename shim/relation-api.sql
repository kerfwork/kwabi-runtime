-- ========================================================================
-- relation-api.sql — the relation cache group through the ABI.
--
-- This is the test for the relation cache slots. It proves an extension can
-- open a relation, read its metadata (name, namespace, relkind, relam,
-- tupledesc, index list), and close it — entirely through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 13 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_relation_control() asserts pg_class == 'wrong_name' and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_relation_open_test(int4);
DROP FUNCTION IF EXISTS kwabi_relation_id_test(int4);
DROP FUNCTION IF EXISTS kwabi_relation_namespace_test(int4);
DROP FUNCTION IF EXISTS kwabi_relation_tupledesc_test(int4);
DROP FUNCTION IF EXISTS kwabi_rel_id_test(int4);
DROP FUNCTION IF EXISTS kwabi_rel_name_test(int4);
DROP FUNCTION IF EXISTS kwabi_rel_namespace_test(int4);
DROP FUNCTION IF EXISTS kwabi_rel_relkind_test(int4);
DROP FUNCTION IF EXISTS kwabi_rel_relam_test(int4);
DROP FUNCTION IF EXISTS kwabi_rel_tupledesc_test(int4);
DROP FUNCTION IF EXISTS kwabi_rel_index_list_test(int4);
DROP FUNCTION IF EXISTS kwabi_relation_control();

CREATE FUNCTION kwabi_relation_open_test(int4)
    RETURNS text AS :'bundle','kwabi_relation_open_test' LANGUAGE C;
CREATE FUNCTION kwabi_relation_id_test(int4)
    RETURNS int4 AS :'bundle','kwabi_relation_id_test' LANGUAGE C;
CREATE FUNCTION kwabi_relation_namespace_test(int4)
    RETURNS int4 AS :'bundle','kwabi_relation_namespace_test' LANGUAGE C;
CREATE FUNCTION kwabi_relation_tupledesc_test(int4)
    RETURNS int4 AS :'bundle','kwabi_relation_tupledesc_test' LANGUAGE C;
CREATE FUNCTION kwabi_rel_id_test(int4)
    RETURNS int4 AS :'bundle','kwabi_rel_id_test' LANGUAGE C;
CREATE FUNCTION kwabi_rel_name_test(int4)
    RETURNS text AS :'bundle','kwabi_rel_name_test' LANGUAGE C;
CREATE FUNCTION kwabi_rel_namespace_test(int4)
    RETURNS int4 AS :'bundle','kwabi_rel_namespace_test' LANGUAGE C;
CREATE FUNCTION kwabi_rel_relkind_test(int4)
    RETURNS text AS :'bundle','kwabi_rel_relkind_test' LANGUAGE C;
CREATE FUNCTION kwabi_rel_relam_test(int4)
    RETURNS int4 AS :'bundle','kwabi_rel_relam_test' LANGUAGE C;
CREATE FUNCTION kwabi_rel_tupledesc_test(int4)
    RETURNS int4 AS :'bundle','kwabi_rel_tupledesc_test' LANGUAGE C;
CREATE FUNCTION kwabi_rel_index_list_test(int4)
    RETURNS int4 AS :'bundle','kwabi_rel_index_list_test' LANGUAGE C;
CREATE FUNCTION kwabi_relation_control()
    RETURNS bool AS :'bundle','kwabi_relation_control' LANGUAGE C;

-- OIDs, identical on 16.15 / 17.11 / 18.6:
--   pg_class = 1259
--   pg_type = 1247
--   pg_proc = 1255

\echo ''
\echo '=== 1. relation_open + relation_name ==='
\echo '   pg_class (1259) must open and be named pg_class'
SELECT kwabi_relation_open_test(1259) = 'pg_class' AS relation_open_name;

\echo ''
\echo '=== 2. relation_open + relation_id ==='
\echo '   pg_class must have OID 1259'
SELECT kwabi_relation_id_test(1259) = 1259 AS relation_id;

\echo ''
\echo '=== 3. relation_open + relation_namespace ==='
\echo '   pg_class must be in pg_catalog (namespace OID 11)'
SELECT kwabi_relation_namespace_test(1259) = 11 AS relation_namespace;

\echo ''
\echo '=== 4. relation_open + relation_tupledesc ==='
\echo '   pg_class must have 36 attributes (natts > 0)'
SELECT kwabi_relation_tupledesc_test(1259) > 0 AS relation_tupledesc;

\echo ''
\echo '=== 5. rel_id ==='
\echo '   pg_class must have OID 1259 via rel_id'
SELECT kwabi_rel_id_test(1259) = 1259 AS rel_id;

\echo ''
\echo '=== 6. rel_name ==='
\echo '   pg_class must be named pg_class via rel_name'
SELECT kwabi_rel_name_test(1259) = 'pg_class' AS rel_name;

\echo ''
\echo '=== 7. rel_namespace ==='
\echo '   pg_class must be in pg_catalog via rel_namespace'
SELECT kwabi_rel_namespace_test(1259) = 11 AS rel_namespace;

\echo ''
\echo '=== 8. rel_relkind ==='
\echo '   pg_class must have relkind r (ordinary table)'
SELECT kwabi_rel_relkind_test(1259) = 'r' AS rel_relkind;

\echo ''
\echo '=== 9. rel_relam ==='
\echo '   pg_class must have a valid relam (heap access method, OID 2)'
SELECT kwabi_rel_relam_test(1259) = 2 AS rel_relam;

\echo ''
\echo '=== 10. rel_tupledesc ==='
\echo '   pg_class must have attributes via rel_tupledesc'
SELECT kwabi_rel_tupledesc_test(1259) > 0 AS rel_tupledesc;

\echo ''
\echo '=== 11. rel_index_list ==='
\echo '   pg_class must have indexes (pg_class_oid_index, etc.)'
SELECT kwabi_rel_index_list_test(1259) > 0 AS rel_index_list;

\echo ''
\echo '=== 12. the backend survived all of the above ==='
SELECT kwabi_relation_id_test(1259) = 1259 AS after_all_checks;

\echo ''
\echo '=== 13. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_relation_control() AS control_should_not_return;

\echo ''
\echo '=== 14. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_relation_id_test(1259) = 1259 AS in_transaction;
COMMIT;

\echo ''
\echo '=== relation-api tests complete ==='
