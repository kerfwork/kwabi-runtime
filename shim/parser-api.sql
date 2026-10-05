-- ========================================================================
-- parser-api.sql — the parser group through the ABI.
--
-- This is the test for the parser slots. It proves an extension can
-- parse expressions, parse type names, and query operator properties
-- entirely through the ABI, with no direct PostgreSQL calls.
--
-- Every check is a boolean, so the harness counts `t` and `f` rather than
-- grepping for a success message.
--
-- ON_ERROR_STOP is off: the negative control (check 8) raises by design.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so
-- the value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_parser_control() asserts a wrong node type and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_parse_expr_test(text);
DROP FUNCTION IF EXISTS kwabi_parse_type_test(text);
DROP FUNCTION IF EXISTS kwabi_oper_left_type(int4);
DROP FUNCTION IF EXISTS kwabi_oper_right_type(int4);
DROP FUNCTION IF EXISTS kwabi_oper_result_type(int4);
DROP FUNCTION IF EXISTS kwabi_oper_is_commutative(int4);
DROP FUNCTION IF EXISTS kwabi_parser_control();

CREATE FUNCTION kwabi_parse_expr_test(sql text)
    RETURNS text AS :'bundle','kwabi_parse_expr_test' LANGUAGE C;
CREATE FUNCTION kwabi_parse_type_test(type_name text)
    RETURNS text AS :'bundle','kwabi_parse_type_test' LANGUAGE C;
CREATE FUNCTION kwabi_oper_left_type(oper_oid int4)
    RETURNS int4 AS :'bundle','kwabi_oper_left_type' LANGUAGE C;
CREATE FUNCTION kwabi_oper_right_type(oper_oid int4)
    RETURNS int4 AS :'bundle','kwabi_oper_right_type' LANGUAGE C;
CREATE FUNCTION kwabi_oper_result_type(oper_oid int4)
    RETURNS int4 AS :'bundle','kwabi_oper_result_type' LANGUAGE C;
CREATE FUNCTION kwabi_oper_is_commutative(oper_oid int4)
    RETURNS bool AS :'bundle','kwabi_oper_is_commutative' LANGUAGE C;
CREATE FUNCTION kwabi_parser_control()
    RETURNS bool AS :'bundle','kwabi_parser_control' LANGUAGE C;

-- OIDs for operators, identical on 16.15 / 17.11 / 18.6 (read from
-- src/include/catalog/pg_operator.dat in each major's source tree):
--   +(int4,int4) = 551 (int4pl)   -(int4,int4) = 555 (int4mi)
--   =(int4,int4) = 96  (int4eq)
--   +(int4,int4) is commutative, -(int4,int4) is not
--
-- These were previously 177/178/176, which are not operators at all. Every
-- operator check fed a nonexistent OID, got InvalidOid back, and failed.
-- int4 OID = 23.

\echo ''
\echo '=== 1. parse_expr: "1 + 1" should produce an OpExpr node ==='
\echo '   The node type name must be "OpExpr"'
SELECT kwabi_parse_expr_test('1 + 1') = 'OpExpr' AS parse_expr_works;

\echo ''
\echo '=== 2. parse_expr: "42" should produce a Const node ==='
SELECT kwabi_parse_expr_test('42') = 'Const' AS parse_expr_const;

\echo ''
\echo '=== 3. parse_type: "integer" should produce a TypeName node ==='
SELECT kwabi_parse_type_test('integer') = 'TypeName' AS parse_type_works;

\echo ''
\echo '=== 4. parse_type: "text" should produce a TypeName node ==='
SELECT kwabi_parse_type_test('text') = 'TypeName' AS parse_type_text;

\echo ''
\echo '=== 5. oper_left_type: +(int4,int4) should have int4 as left type ==='
\echo '   int4 OID = 23'
SELECT kwabi_oper_left_type(551) = 23 AS oper_left_type_int4;

\echo ''
\echo '=== 6. oper_right_type: +(int4,int4) should have int4 as right type ==='
SELECT kwabi_oper_right_type(551) = 23 AS oper_right_type_int4;

\echo ''
\echo '=== 7. oper_result_type: +(int4,int4) should have int4 as result type ==='
SELECT kwabi_oper_result_type(551) = 23 AS oper_result_type_int4;

\echo ''
\echo '=== 8. oper_is_commutative: +(int4,int4) should be commutative ==='
SELECT kwabi_oper_is_commutative(551) = true AS oper_is_commutative_true;

\echo ''
\echo '=== 9. oper_is_commutative: -(int4,int4) should NOT be commutative ==='
SELECT kwabi_oper_is_commutative(555) = false AS oper_is_commutative_false;

\echo ''
\echo '=== 10. THE NEGATIVE CONTROL: a wrong node type must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_parser_control() AS control_should_not_return;

\echo ''
\echo '=== 11. the backend survived all of the above ==='
SELECT kwabi_parse_expr_test('1 + 1') = 'OpExpr' AS after_control;

\echo ''
\echo '=== 12. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_parse_expr_test('1 + 1') = 'OpExpr' AS in_transaction;
COMMIT;

\echo ''
\echo '=== parser-api tests complete ==='
