CREATE OR REPLACE LIBRARY ODBCLoaderLib AS '/opt/vertica/packages/odbc-loader/lib/ODBCLoader.so';
CREATE OR REPLACE PARSER ODBCLoader AS LANGUAGE 'C++' NAME 'ODBCLoaderFactory' LIBRARY ODBCLoaderLib FENCED;
CREATE OR REPLACE SOURCE ODBCSource AS LANGUAGE 'C++' NAME 'ODBCSourceFactory' LIBRARY ODBCLoaderLib FENCED;
--CREATE OR REPLACE PARSER ODBCLoader AS LANGUAGE 'C++' NAME 'ODBCLoaderFactory' LIBRARY ODBCLoaderLib NOT FENCED;
--CREATE OR REPLACE SOURCE ODBCSource AS LANGUAGE 'C++' NAME 'ODBCSourceFactory' LIBRARY ODBCLoaderLib NOT FENCED;
GRANT EXECUTE ON SOURCE public.ODBCSource() TO public;
GRANT EXECUTE ON PARSER public.ODBCLoader() TO public;
