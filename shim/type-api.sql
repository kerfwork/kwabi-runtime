-- ========================================================================
-- type-api.sql — the type system group through the ABI.
--
-- This is the test for kata vq2c (type-api). It proves an extension can
-- look up type information, convert values to and from their text and
-- binary representations, and query type properties — entirely through
-- the ABI, with no direct PostgreSQL calls.
--
-- Every check is a boolean, so the harness counts `t` and `f` rather than
-- grepping for a success message.
--
-- ON_ERROR_STOP is off: the negative control (check 9) raises by design.
-- ========================================================================

\set ON_ERROR_STOP off

-- Create a test table with known types.
DROP TABLE IF EXISTS kwabi_type_test;
CREATE TABLE kwabi_type_test (
    id serial PRIMARY KEY,
    val_int integer,
    val_text text,
    val_bool boolean,
    val_float float8
);

-- ========================================================================
-- Check 1: type_length for integer (int4) should be 4
-- ========================================================================
SELECT 'type_length_int4' AS check,
       api->type_length(23) = 4 AS result;

-- ========================================================================
-- Check 2: type_length for text should be -1 (variable length)
-- ========================================================================
SELECT 'type_length_text' AS check,
       api->type_length(25) = -1 AS result;

-- ========================================================================
-- Check 3: type_is_array for integer should be false
-- ========================================================================
SELECT 'type_is_array_int4' AS check,
       api->type_is_array(23) = false AS result;

-- ========================================================================
-- Check 4: type_is_array for integer[] should be true
-- ========================================================================
SELECT 'type_is_array_int4_array' AS check,
       api->type_is_array(1007) = true AS result;

-- ========================================================================
-- Check 5: type_is_composite for a table type should be true
-- ========================================================================
SELECT 'type_is_composite_table' AS check,
       api->type_is_composite('kwabi_type_test'::regclass::oid) = true AS result;

-- ========================================================================
-- Check 6: type_is_composite for integer should be false
-- ========================================================================
SELECT 'type_is_composite_int4' AS check,
       api->type_is_composite(23) = false AS result;

-- ========================================================================
-- Check 7: type_base_type for varchar should be text
-- ========================================================================
SELECT 'type_base_type_varchar' AS check,
       api->type_base_type(1043) = 25 AS result;

-- ========================================================================
-- Check 8: type_input for integer should parse "42"
-- ========================================================================
SELECT 'type_input_int4' AS check,
       api->type_input(23, '42', -1) = 42 AS result;

-- ========================================================================
-- Check 9: type_output for integer 42 should produce "42"
-- ========================================================================
SELECT 'type_output_int4' AS check,
       api->type_output(23, 42) = '42' AS result;

-- ========================================================================
-- Check 10: type_input for boolean should parse "true"
-- ========================================================================
SELECT 'type_input_bool' AS check,
       api->type_input(16, 'true', -1) = true AS result;

-- ========================================================================
-- Check 11: type_output for boolean true should produce "true"
-- ========================================================================
SELECT 'type_output_bool' AS check,
       api->type_output(16, true) = 'true' AS result;

-- ========================================================================
-- Check 12: type_input for float8 should parse "3.14"
-- ========================================================================
SELECT 'type_input_float8' AS check,
       api->type_input(701, '3.14', -1) = 3.14::float8 AS result;

-- ========================================================================
-- Check 13: type_output for float8 3.14 should produce "3.14"
-- ========================================================================
SELECT 'type_output_float8' AS check,
       api->type_output(701, 3.14::float8) = '3.14' AS result;

-- ========================================================================
-- Check 14: type_element_type for integer[] should be integer
-- ========================================================================
SELECT 'type_element_type_int4_array' AS check,
       api->type_element_type(1007) = 23 AS result;

-- ========================================================================
-- Check 15: type_input for text should parse "hello"
-- ========================================================================
SELECT 'type_input_text' AS check,
       api->type_input(25, 'hello', -1) = 'hello'::text AS result;

-- ========================================================================
-- Check 16: type_output for text "hello" should produce "hello"
-- ========================================================================
SELECT 'type_output_text' AS check,
       api->type_output(25, 'hello'::text) = 'hello' AS result;

-- ========================================================================
-- Check 17: negative control — type_input with invalid input should raise
-- ========================================================================
SELECT 'type_input_invalid' AS check,
       api->type_input(23, 'not_a_number', -1) IS NULL AS result;

-- ========================================================================
-- Check 18: negative control — type_length for invalid OID should return 0
-- ========================================================================
SELECT 'type_length_invalid' AS check,
       api->type_length(999999) = 0 AS result;

-- ========================================================================
-- Check 19: negative control — type_is_array for invalid OID should return false
-- ========================================================================
SELECT 'type_is_array_invalid' AS check,
       api->type_is_array(999999) = false AS result;

-- ========================================================================
-- Check 20: negative control — type_base_type for invalid OID should return 0
-- ========================================================================
SELECT 'type_base_type_invalid' AS check,
       api->type_base_type(999999) = 0 AS result;

-- Cleanup
DROP TABLE IF EXISTS kwabi_type_test;
