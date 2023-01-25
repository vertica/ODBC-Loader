-- Set output to a fixed timezone regardless of where this is being tested
set time zone to 'EST';

-- Create the corresponding table in Vertica
CREATE TABLE test_vertica (i integer, b boolean, f float, v varchar(32), c char(32), lv varchar(9999), bn binary(32), vb varbinary(32), lvb varbinary(999), d date, t time, ts timestamp, tz timetz, tsz timestamptz, n numeric(18,4));

-- Copy from MySQL into Vertica
COPY test_vertica WITH SOURCE ODBCSource() PARSER ODBCLoader(connect='DSN=MySQL', query='SELECT * FROM testdb.test_source;');

-- Verify thae output
SELECT i,b,f,v,trim(c::varchar) as c,lv,bn::binary(8) as bn,vb,lvb,d,t,ts,tz,tsz,n FROM test_vertica ORDER BY i,b,f,v;

-- Clean up
DROP TABLE test_vertica;
