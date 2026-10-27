\timing off

-- Multi-threaded fetch: thread_count / split_column / split_method.
-- Slice order is not deterministic, so every check below is an order-independent
-- aggregate over testdb.test_source (9 data rows plus 1 all-NULL row). A dropped
-- row, a duplicated row, or a NULL key claimed by two slices moves at least one
-- of the four totals.
-- These totals track the shared testdb.test_source fixture created by
-- .github/workflows/ci.yml; editing that fixture means updating this test too.

-- Target table reused (and truncated) by every load below
CREATE TABLE mt_target (i integer, v varchar(32));

-- Test 1: no threading parameters; the original single-connection path
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 2: range split across 4 workers; the NULL key rides on the last slice only
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=4, split_column='i', split_method='range');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 3: modulo split, split_method given in uppercase to check case folding
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=4, split_column='i', split_method='MODULO');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 4: 16 threads over 9 distinct keys; empty slices are dropped, not run
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=16, split_column='i');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 5: rowset=1 forces many one-row batches, so workers block on a full queue
-- while the main thread drains it across several process() calls
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=2, split_column='i', rowset=1);
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 6: thread_count=1 keeps a valid split_column on the single-connection path
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=1, split_column='i');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 7: split_column absent from the remote query; the probe fails and the load
-- falls back to one connection instead of raising
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=4, split_column='no_such_column');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 8: non-integer split_column; MIN/MAX will not parse as whole integers
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=4, split_column='v');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 9: thread_count with nothing to split on is ignored
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=4);
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 10: an unrecognized split_method uses the portable range split
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=4, split_column='i', split_method='bogus');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Tests 11-13 are the regression guard for the MOD() sign fix. 'i - 7' shifts the
-- key range to -6..2, so remainders cover 0..3 with mixed signs and the key sum is
-- non-zero; mt_target.i receives that shifted key. All three paths must agree:
-- nrows=10, nn_i=9, sum_i=-18, len_v=54.

-- Test 11: negative keys, no split; these are the reference totals
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i - 7 AS k, v FROM testdb.test_source;');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 12: negative keys, range split
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i - 7 AS k, v FROM testdb.test_source;', thread_count=4, split_column='k', split_method='range');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 13: negative keys, modulo split. Five of the nine keys have a negative
-- remainder; without MOD(MOD(k,N)+N,N) they match no slice and disappear
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i - 7 AS k, v FROM testdb.test_source;', thread_count=4, split_column='k', split_method='modulo');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v FROM mt_target;
TRUNCATE TABLE mt_target;

-- Test 14: thread_count outside 1..64 is rejected in setup() and loads nothing
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=0, split_column='i');
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;', thread_count=65, split_column='i');
SELECT count(*) AS nrows FROM mt_target;

-- Test 15: threaded fetch through an External Table, which re-enters process()
-- until every worker has finished and the queue is drained
CREATE EXTERNAL TABLE public.mt_people (
    id INTEGER,
    name VARCHAR(20)
) AS COPY WITH
    SOURCE ODBCSource()
    PARSER ODBCLoader(
        connect='DSN=MySQL',
        query='SELECT * FROM testdb.people',
        thread_count=3,
        split_column='id'
);

SELECT id, name FROM public.mt_people ORDER BY id;

-- Test 16: a worker that connects and then dies part way through its slice. The
-- geometry conversion is only evaluated for i > 6, so slice 0 (which setup() also
-- runs to derive column metadata) and slice 1 succeed while the last slice fails
-- on the remote side. This proves the runtime path no other test reaches:
-- captureWorkerError -> re-raise on the main thread -> Vertica rolls the COPY
-- back, so none of the rows the healthy workers already emitted are committed.
COPY mt_target WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, ST_ASTEXT(ST_GEOMFROMTEXT(CASE WHEN i > 6 THEN ''oops'' ELSE ''POINT(1 1)'' END)) AS v FROM testdb.test_source;', thread_count=3, split_column='i');
SELECT count(*) AS nrows FROM mt_target;

-- Tests 17-28 are the regression guard for VTA-118933. Each fixture is built inline
-- on the MySQL side and loaded four ways (single, default, range, modulo); all four
-- must list every row. Listings are ordered by payload, since SUM over extreme keys
-- would overflow.
CREATE TABLE mt_bigint (id NUMERIC(19,0), payload INT);
CREATE TABLE mt_numeric (id NUMERIC(38,0), payload INT);
CREATE TABLE mt_fraction (id NUMERIC(38,3), payload INT);

-- Test 17: extreme BIGINT keys spanning the full signed 64-bit range, single connection; the reference listing
-- Range offsets above LLONG_MAX used to wrap and drop the slice holding -1. The target
-- is NUMERIC(19,0) because Vertica INT stores -9223372036854775808 as NULL.
COPY mt_bigint WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-9223372036854775808 AS SIGNED) AS id, 1 AS payload UNION ALL SELECT CAST(-9223372036854775807 AS SIGNED), 2 UNION ALL SELECT CAST(-1 AS SIGNED), 3 UNION ALL SELECT CAST(0 AS SIGNED), 4 UNION ALL SELECT CAST(1 AS SIGNED), 5 UNION ALL SELECT CAST(9223372036854775806 AS SIGNED), 6 UNION ALL SELECT CAST(9223372036854775807 AS SIGNED), 7;');
SELECT id, payload FROM mt_bigint ORDER BY payload;
SELECT count(*) AS nrows FROM mt_bigint;
TRUNCATE TABLE mt_bigint;

-- Test 18: extreme BIGINT keys spanning the full signed 64-bit range, default split (no split_method)
COPY mt_bigint WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-9223372036854775808 AS SIGNED) AS id, 1 AS payload UNION ALL SELECT CAST(-9223372036854775807 AS SIGNED), 2 UNION ALL SELECT CAST(-1 AS SIGNED), 3 UNION ALL SELECT CAST(0 AS SIGNED), 4 UNION ALL SELECT CAST(1 AS SIGNED), 5 UNION ALL SELECT CAST(9223372036854775806 AS SIGNED), 6 UNION ALL SELECT CAST(9223372036854775807 AS SIGNED), 7;', thread_count=4, split_column='id');
SELECT id, payload FROM mt_bigint ORDER BY payload;
SELECT count(*) AS nrows FROM mt_bigint;
TRUNCATE TABLE mt_bigint;

-- Test 19: extreme BIGINT keys spanning the full signed 64-bit range, explicit range split
COPY mt_bigint WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-9223372036854775808 AS SIGNED) AS id, 1 AS payload UNION ALL SELECT CAST(-9223372036854775807 AS SIGNED), 2 UNION ALL SELECT CAST(-1 AS SIGNED), 3 UNION ALL SELECT CAST(0 AS SIGNED), 4 UNION ALL SELECT CAST(1 AS SIGNED), 5 UNION ALL SELECT CAST(9223372036854775806 AS SIGNED), 6 UNION ALL SELECT CAST(9223372036854775807 AS SIGNED), 7;', thread_count=4, split_column='id', split_method='range');
SELECT id, payload FROM mt_bigint ORDER BY payload;
SELECT count(*) AS nrows FROM mt_bigint;
TRUNCATE TABLE mt_bigint;

-- Test 20: extreme BIGINT keys spanning the full signed 64-bit range, modulo split
COPY mt_bigint WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-9223372036854775808 AS SIGNED) AS id, 1 AS payload UNION ALL SELECT CAST(-9223372036854775807 AS SIGNED), 2 UNION ALL SELECT CAST(-1 AS SIGNED), 3 UNION ALL SELECT CAST(0 AS SIGNED), 4 UNION ALL SELECT CAST(1 AS SIGNED), 5 UNION ALL SELECT CAST(9223372036854775806 AS SIGNED), 6 UNION ALL SELECT CAST(9223372036854775807 AS SIGNED), 7;', thread_count=4, split_column='id', split_method='modulo');
SELECT id, payload FROM mt_bigint ORDER BY payload;
SELECT count(*) AS nrows FROM mt_bigint;
TRUNCATE TABLE mt_bigint;

-- Test 21: DECIMAL(38,0) keys whose MIN/MAX overflow long long, single connection; the reference listing
-- strtoll() clamped the bounds to LLONG_MIN/LLONG_MAX and dropped 3 of 5 rows; the
-- split must now fall back to one connection.
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-99999999999999999999999999999999999999 AS DECIMAL(38,0)) AS id, 1 AS payload UNION ALL SELECT CAST(-100 AS DECIMAL(38,0)), 2 UNION ALL SELECT CAST(0 AS DECIMAL(38,0)), 3 UNION ALL SELECT CAST(100 AS DECIMAL(38,0)), 4 UNION ALL SELECT CAST(99999999999999999999999999999999999999 AS DECIMAL(38,0)), 5;');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Test 22: DECIMAL(38,0) keys whose MIN/MAX overflow long long, default split (no split_method)
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-99999999999999999999999999999999999999 AS DECIMAL(38,0)) AS id, 1 AS payload UNION ALL SELECT CAST(-100 AS DECIMAL(38,0)), 2 UNION ALL SELECT CAST(0 AS DECIMAL(38,0)), 3 UNION ALL SELECT CAST(100 AS DECIMAL(38,0)), 4 UNION ALL SELECT CAST(99999999999999999999999999999999999999 AS DECIMAL(38,0)), 5;', thread_count=4, split_column='id');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Test 23: DECIMAL(38,0) keys whose MIN/MAX overflow long long, explicit range split
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-99999999999999999999999999999999999999 AS DECIMAL(38,0)) AS id, 1 AS payload UNION ALL SELECT CAST(-100 AS DECIMAL(38,0)), 2 UNION ALL SELECT CAST(0 AS DECIMAL(38,0)), 3 UNION ALL SELECT CAST(100 AS DECIMAL(38,0)), 4 UNION ALL SELECT CAST(99999999999999999999999999999999999999 AS DECIMAL(38,0)), 5;', thread_count=4, split_column='id', split_method='range');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Test 24: DECIMAL(38,0) keys whose MIN/MAX overflow long long, modulo split
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(-99999999999999999999999999999999999999 AS DECIMAL(38,0)) AS id, 1 AS payload UNION ALL SELECT CAST(-100 AS DECIMAL(38,0)), 2 UNION ALL SELECT CAST(0 AS DECIMAL(38,0)), 3 UNION ALL SELECT CAST(100 AS DECIMAL(38,0)), 4 UNION ALL SELECT CAST(99999999999999999999999999999999999999 AS DECIMAL(38,0)), 5;', thread_count=4, split_column='id', split_method='modulo');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Test 25: DOUBLE keys 0, 0.5, 1, 1.5, 2, single connection; the reference listing
-- MIN/MAX print as whole numbers, but the DOUBLE type must force the fallback;
-- otherwise 0.5 and 1.5 fall between the integer BETWEEN ranges.
COPY mt_fraction WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT 0e0 AS id, 1 AS payload UNION ALL SELECT 0.5e0, 2 UNION ALL SELECT 1e0, 3 UNION ALL SELECT 1.5e0, 4 UNION ALL SELECT 2e0, 5;');
SELECT id, payload FROM mt_fraction ORDER BY payload;
SELECT count(*) AS nrows FROM mt_fraction;
TRUNCATE TABLE mt_fraction;

-- Test 26: DOUBLE keys 0, 0.5, 1, 1.5, 2, default split (no split_method)
COPY mt_fraction WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT 0e0 AS id, 1 AS payload UNION ALL SELECT 0.5e0, 2 UNION ALL SELECT 1e0, 3 UNION ALL SELECT 1.5e0, 4 UNION ALL SELECT 2e0, 5;', thread_count=4, split_column='id');
SELECT id, payload FROM mt_fraction ORDER BY payload;
SELECT count(*) AS nrows FROM mt_fraction;
TRUNCATE TABLE mt_fraction;

-- Test 27: DOUBLE keys 0, 0.5, 1, 1.5, 2, explicit range split
COPY mt_fraction WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT 0e0 AS id, 1 AS payload UNION ALL SELECT 0.5e0, 2 UNION ALL SELECT 1e0, 3 UNION ALL SELECT 1.5e0, 4 UNION ALL SELECT 2e0, 5;', thread_count=4, split_column='id', split_method='range');
SELECT id, payload FROM mt_fraction ORDER BY payload;
SELECT count(*) AS nrows FROM mt_fraction;
TRUNCATE TABLE mt_fraction;

-- Test 28: DOUBLE keys 0, 0.5, 1, 1.5, 2, modulo split
COPY mt_fraction WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT 0e0 AS id, 1 AS payload UNION ALL SELECT 0.5e0, 2 UNION ALL SELECT 1e0, 3 UNION ALL SELECT 1.5e0, 4 UNION ALL SELECT 2e0, 5;', thread_count=4, split_column='id', split_method='modulo');
SELECT id, payload FROM mt_fraction ORDER BY payload;
SELECT count(*) AS nrows FROM mt_fraction;
TRUNCATE TABLE mt_fraction;

-- Test 29: whole-valued DECIMAL(10,0) keys 1-5, single connection; the reference listing
-- Only native integer keys are split; DECIMAL falls back even when every value is whole.
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(1 AS DECIMAL(10,0)) AS id, 1 AS payload UNION ALL SELECT CAST(2 AS DECIMAL(10,0)), 2 UNION ALL SELECT CAST(3 AS DECIMAL(10,0)), 3 UNION ALL SELECT CAST(4 AS DECIMAL(10,0)), 4 UNION ALL SELECT CAST(5 AS DECIMAL(10,0)), 5;');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Test 30: whole-valued DECIMAL(10,0) keys 1-5, default split (no split_method)
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(1 AS DECIMAL(10,0)) AS id, 1 AS payload UNION ALL SELECT CAST(2 AS DECIMAL(10,0)), 2 UNION ALL SELECT CAST(3 AS DECIMAL(10,0)), 3 UNION ALL SELECT CAST(4 AS DECIMAL(10,0)), 4 UNION ALL SELECT CAST(5 AS DECIMAL(10,0)), 5;', thread_count=4, split_column='id');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Test 31: whole-valued DECIMAL(10,0) keys 1-5, explicit range split
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(1 AS DECIMAL(10,0)) AS id, 1 AS payload UNION ALL SELECT CAST(2 AS DECIMAL(10,0)), 2 UNION ALL SELECT CAST(3 AS DECIMAL(10,0)), 3 UNION ALL SELECT CAST(4 AS DECIMAL(10,0)), 4 UNION ALL SELECT CAST(5 AS DECIMAL(10,0)), 5;', thread_count=4, split_column='id', split_method='range');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Test 32: whole-valued DECIMAL(10,0) keys 1-5, modulo split
COPY mt_numeric WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT CAST(1 AS DECIMAL(10,0)) AS id, 1 AS payload UNION ALL SELECT CAST(2 AS DECIMAL(10,0)), 2 UNION ALL SELECT CAST(3 AS DECIMAL(10,0)), 3 UNION ALL SELECT CAST(4 AS DECIMAL(10,0)), 4 UNION ALL SELECT CAST(5 AS DECIMAL(10,0)), 5;', thread_count=4, split_column='id', split_method='modulo');
SELECT id, payload FROM mt_numeric ORDER BY payload;
SELECT count(*) AS nrows FROM mt_numeric;
TRUNCATE TABLE mt_numeric;

-- Clean up
DROP TABLE public.mt_people;
DROP TABLE mt_target;
DROP TABLE mt_bigint;
DROP TABLE mt_numeric;
DROP TABLE mt_fraction;
