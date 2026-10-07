-- ========================================================================
-- type-api.sql — the type system group through the ABI.
--
-- This is the test for the type system slots. It proves an extension can
-- look up type information, convert values to and from their text
-- representations, and query type properties — entirely through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: the negative control raises by design.
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_type_control() asserts type_length(23) == 999 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as relation-api.sql and fmgr-api.sql.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

-- Create a test table with known types.
DROP TABLE IF EXISTS kwabi_type_test;
CREATE TABLE kwabi_type_test (
    id serial PRIMARY KEY,
    val_int integer,
    val_text text,
    val_bool boolean,
    val_float float8
);

-- Create a domain type to properly test base_type (varchar is NOT a domain).
DROP DOMAIN IF EXISTS kwabi_test_domain;
CREATE DOMAIN kwabi_test_domain AS text;

-- Register the SQL-callable wrappers.
DROP FUNCTION IF EXISTS kwabi_type_length(int4);
DROP FUNCTION IF EXISTS kwabi_type_is_array(int4);
DROP FUNCTION IF EXISTS kwabi_type_is_composite(int4);
DROP FUNCTION IF EXISTS kwabi_type_element_type(int4);
DROP FUNCTION IF EXISTS kwabi_type_base_type(int4);
DROP FUNCTION IF EXISTS kwabi_type_input(int4, text, int4);
DROP FUNCTION IF EXISTS kwabi_type_output(int4, text);
DROP FUNCTION IF EXISTS kwabi_type_send(int4, text);
DROP FUNCTION IF EXISTS kwabi_type_control();

CREATE FUNCTION kwabi_type_length(int4)
    RETURNS int4 AS :'bundle','kwabi_type_length' LANGUAGE C;
CREATE FUNCTION kwabi_type_is_array(int4)
    RETURNS bool AS :'bundle','kwabi_type_is_array' LANGUAGE C;
CREATE FUNCTION kwabi_type_is_composite(int4)
    RETURNS bool AS :'bundle','kwabi_type_is_composite' LANGUAGE C;
CREATE FUNCTION kwabi_type_element_type(int4)
    RETURNS int4 AS :'bundle','kwabi_type_element_type' LANGUAGE C;
CREATE FUNCTION kwabi_type_base_type(int4)
    RETURNS int4 AS :'bundle','kwabi_type_base_type' LANGUAGE C;
CREATE FUNCTION kwabi_type_input(int4, text, int4)
    RETURNS text AS :'bundle','kwabi_type_input' LANGUAGE C;
CREATE FUNCTION kwabi_type_output(int4, text)
    RETURNS text AS :'bundle','kwabi_type_output' LANGUAGE C;
CREATE FUNCTION kwabi_type_send(int4, text)
    RETURNS bytea AS :'bundle','kwabi_type_send' LANGUAGE C;
CREATE FUNCTION kwabi_type_control()
    RETURNS bool AS :'bundle','kwabi_type_control' LANGUAGE C;

-- OIDs, identical on 16 / 17 / 18:
--   int4 = 23, text = 25, bool = 16, float8 = 701, varchar = 1043, int4[] = 1007

\echo ''
\echo '=== 1. type_length for int4 (23) should be 4 ==='
SELECT kwabi_type_length(23) = 4 AS type_length_int4;

\echo ''
\echo '=== 2. type_length for text (25) should be -1 (varlena) ==='
SELECT kwabi_type_length(25) = -1 AS type_length_text;

\echo ''
\echo '=== 3. type_is_array for int4 (23) should be false ==='
SELECT kwabi_type_is_array(23) = false AS type_is_array_int4;

\echo ''
\echo '=== 4. type_is_array for int4[] (1007) should be true ==='
SELECT kwabi_type_is_array(1007) = true AS type_is_array_int4_array;

\echo ''
\echo '=== 5. type_element_type for int4[] (1007) should be int4 (23) ==='
SELECT kwabi_type_element_type(1007) = 23 AS type_element_type_int4_array;

\echo ''
\echo '=== 6. type_is_composite for a table rowtype should be true ==='
SELECT kwabi_type_is_composite('kwabi_type_test'::regtype::oid::int4) = true AS type_is_composite_table;

\echo ''
\echo '=== 7. type_is_composite for int4 (23) should be false ==='
SELECT kwabi_type_is_composite(23) = false AS type_is_composite_int4;

\echo ''
\echo '=== 8. type_base_type for a domain over text should be text (25) ==='
SELECT kwabi_type_base_type((SELECT oid FROM pg_type WHERE typname = 'kwabi_test_domain')::int4) = 25 AS type_base_type_domain;

\echo ''
\echo '=== 9. type_base_type for varchar (1043) should be 1043 (not a domain) ==='
SELECT kwabi_type_base_type(1043) = 1043 AS type_base_type_varchar;

\echo ''
\echo '=== 10. type_input for int4 (23) should parse "42" ==='
SELECT kwabi_type_input(23, '42', -1) = '42' AS type_input_int4;

\echo ''
\echo '=== 11. type_output for int4 (23) value "42" should produce "42" ==='
SELECT kwabi_type_output(23, '42') = '42' AS type_output_int4;

\echo ''
\echo '=== 12. type_input for bool (16) should parse "true" to "t" ==='
SELECT kwabi_type_input(16, 'true', -1) = 't' AS type_input_bool;

\echo ''
\echo '=== 13. type_output for bool (16) value "true" should produce "t" ==='
SELECT kwabi_type_output(16, 'true') = 't' AS type_output_bool;

\echo ''
\echo '=== 14. type_input for float8 (701) should parse "3.14" ==='
SELECT kwabi_type_input(701, '3.14', -1) = '3.14' AS type_input_float8;

\echo ''
\echo '=== 15. type_output for float8 (701) value "3.14" should produce "3.14" ==='
SELECT kwabi_type_output(701, '3.14') = '3.14' AS type_output_float8;

\echo ''
\echo '=== 16. type_input for text (25) should parse "hello" ==='
SELECT kwabi_type_input(25, 'hello', -1) = 'hello' AS type_input_text;

\echo ''
\echo '=== 17. type_output for text (25) value "hello" should produce "hello" ==='
SELECT kwabi_type_output(25, 'hello') = 'hello' AS type_output_text;

\echo ''
\echo '=== 18. type_send for int4 (23) value "42" should produce 0x0000002a ==='
SELECT kwabi_type_send(23, '42') = '\x0000002a'::bytea AS type_send_int4;

\echo ''
\echo '=== 19. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_type_control() AS control_should_not_return;

\echo ''
\echo '=== 20. the backend survived all of the above ==='
SELECT kwabi_type_length(23) = 4 AS after_all_checks;

\echo ''
\echo '=== type-api tests complete ==='

-- Cleanup
DROP TABLE IF EXISTS kwabi_type_test;
DROP DOMAIN IF EXISTS kwabi_test_domain;
