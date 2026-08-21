\timing off

-- Set output to a fixed timezone regardless of where this is being tested
set time zone to 'EST';

-- Multi-threaded fetch: thread_count / split_column / split_method.
-- Slice order is not deterministic, so every check below is an order-independent
-- aggregate over testdb.test_source (9 data rows plus 1 all-NULL row). A dropped
-- row, a duplicated row, or a NULL key claimed by two slices moves at least one
-- of the four totals.

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

-- Clean up
DROP TABLE public.mt_people;
DROP TABLE mt_target;
