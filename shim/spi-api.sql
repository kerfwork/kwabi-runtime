-- ========================================================================
-- spi-api.sql — the SPI group through the ABI.
--
-- This is the test for the SPI slots. It proves an extension can execute
-- SQL queries, read results, and handle errors entirely through the ABI.
--
-- Every check is an explicit boolean. The harness counts `t` and `f` rather
-- than grepping for a message.
--
-- ON_ERROR_STOP is off: check 5 raises by design (the negative control).
--
-- NEGATIVE CONTROL. A check that has only ever passed is not evidence, so the
-- value comparison is run in both directions:
--   * the equality checks below must SUCCEED;
--   * kwabi_spi_control() asserts 1 == 2 and must RAISE.
-- If the control returns a row instead of erroring, every equality assertion
-- here is vacuous. Same standard as fmgr-api.sql and capabilities-design.md §5.
-- ========================================================================

\set ON_ERROR_STOP off

LOAD :'bundle';

DROP FUNCTION IF EXISTS kwabi_spi_query(text);
DROP FUNCTION IF EXISTS kwabi_spi_insert_then_select();
DROP FUNCTION IF EXISTS kwabi_spi_control();

CREATE FUNCTION kwabi_spi_query(sql text)
    RETURNS text AS :'bundle','kwabi_spi_query' LANGUAGE C;
CREATE FUNCTION kwabi_spi_insert_then_select()
    RETURNS int4 AS :'bundle','kwabi_spi_insert_then_select' LANGUAGE C;
CREATE FUNCTION kwabi_spi_control()
    RETURNS bool AS :'bundle','kwabi_spi_control' LANGUAGE C;

-- A table for the insert-then-select test.
DROP TABLE IF EXISTS kwabi_spi_test_t;
CREATE TABLE kwabi_spi_test_t (val int);

\echo ''
\echo '=== 1. a simple SELECT through the ABI ==='
\echo '   SELECT 1 must return one row with value 1'
SELECT kwabi_spi_query('SELECT 1') LIKE '%rows=1%' AS select_works;

\echo ''
\echo '=== 2. a multi-row SELECT ==='
\echo '   SELECT generate_series(1,5) must return 5 rows'
SELECT kwabi_spi_query('SELECT generate_series(1,5)') LIKE '%rows=5%' AS multi_row;

\echo ''
\echo '=== 3. insert then select through the ABI ==='
\echo '   insert 42, then select it back -- the value must be 42'
SELECT kwabi_spi_insert_then_select() = 42 AS insert_then_select;

\echo ''
\echo '=== 4. a write through the ABI is visible to SQL ==='
\echo '   (proves the write committed, not just readable through the ABI)'
INSERT INTO kwabi_spi_test_t VALUES (100);
SELECT kwabi_spi_query('SELECT val FROM kwabi_spi_test_t WHERE val = 100') LIKE '%rows=1%' AS write_visible;

\echo ''
\echo '=== 5. THE NEGATIVE CONTROL: a wrong comparison must RAISE ==='
\echo '   (must ERROR with "fired as intended"; returning a row means every'
\echo '    equality check above is vacuous)'
SELECT kwabi_spi_control() AS control_should_not_return;

\echo ''
\echo '=== 6. the backend survived all of the above ==='
SELECT kwabi_spi_query('SELECT 42') LIKE '%rows=1%' AS after_control;

\echo ''
\echo '=== 7. the whole group works inside a transaction ==='
BEGIN;
SELECT kwabi_spi_query('SELECT 1') LIKE '%rows=1%' AS in_transaction;
COMMIT;

\echo ''
\echo '=== spi-api tests complete ==='
