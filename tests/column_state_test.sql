\timing off

-- VTA-118540 regression: column state must not leak between loads.
-- The target column count and the fetched-to-target column mapping used to be
-- process-wide globals, so the first load in a session fixed them for every load
-- after it. A narrower load then hit an invalid setter exception, and a wider one
-- read past the end of the stale mapping. Every statement below runs in ONE vsql
-- session on purpose: the width changes between loads are what is being tested.
-- Uses the shared testdb.test_source (10 rows) and testdb.people (3 rows)

-- 7-column and 2-column targets, reused (and truncated) by the loads below
CREATE TABLE cs_wide (i integer, v varchar(32), f float, c char(32), lv varchar(9999), d date, n numeric(18,4));
CREATE TABLE cs_narrow (id integer, name varchar(20));

-- Test 1: wide load; first load of the session. nn_n proves the last column is mapped
COPY cs_wide WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v, f, c, lv, d, n FROM testdb.test_source;');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v, count(n) AS nn_n FROM cs_wide;
TRUNCATE TABLE cs_wide;

-- Test 2: narrow load right after a wide one, no reconnect. Before the fix the
-- stale width (7) made the NULL-reset loop write past the 2 target columns
COPY cs_narrow WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT id, name FROM testdb.people;');
SELECT id, name FROM cs_narrow ORDER BY id;
TRUNCATE TABLE cs_narrow;

-- Test 3: wide again after narrow
COPY cs_wide WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v, f, c, lv, d, n FROM testdb.test_source;');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v, count(n) AS nn_n FROM cs_wide;
TRUNCATE TABLE cs_wide;

-- Test 4: narrow again; the 7 -> 2 -> 7 -> 2 sequence must keep working
COPY cs_narrow WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT id, name FROM testdb.people;');
SELECT id, name FROM cs_narrow ORDER BY id;
TRUNCATE TABLE cs_narrow;

-- Test 5: External Table, full and projected. The projected query sets a
-- non-identity mapping through __query_col_idx__ (fetched column 0 -> target 1)
CREATE EXTERNAL TABLE public.cs_epeople (
    id INTEGER,
    name VARCHAR(20)
) AS COPY WITH
    SOURCE ODBCSource()
    PARSER ODBCLoader(
        connect='DSN=MySQL',
        query='SELECT * FROM testdb.people'
);

SELECT id, name FROM public.cs_epeople ORDER BY id;
SELECT name FROM public.cs_epeople ORDER BY name;

-- Test 6: plain wide COPY right after the projected query. Before the fix the
-- 1-entry projection mapping was reused for a 7-column load
COPY cs_wide WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v, f, c, lv, d, n FROM testdb.test_source;');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v, count(n) AS nn_n FROM cs_wide;
TRUNCATE TABLE cs_wide;

-- Test 7: remote query returns fewer columns than the target; the trailing
-- target columns must load as NULL (nn_n = 0)
COPY cs_wide WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT i, v FROM testdb.test_source;');
SELECT count(*) AS nrows, count(i) AS nn_i, sum(i) AS sum_i, sum(length(v)) AS len_v, count(n) AS nn_n FROM cs_wide;
TRUNCATE TABLE cs_wide;

-- Test 8: a plain narrow COPY after the External Table and wide loads still maps correctly
COPY cs_narrow WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT id, name FROM testdb.people;');
SELECT id, name FROM cs_narrow ORDER BY id;

-- Clean up
DROP TABLE public.cs_epeople;
DROP TABLE cs_narrow;
DROP TABLE cs_wide;
