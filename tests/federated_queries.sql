\timing off

-- Set output to a fixed timezone regardless of where this is being tested
set time zone to 'EST';

-- =====================================================
-- Federated Queries Test Case
-- =====================================================
-- This test demonstrates the use of ODBCLoader for federated queries
-- allowing Vertica to query external databases (e.g., MySQL) while
-- leveraging predicate pushdown and column pruning to minimize data movement

-- Create an External Table in Vertica that acts as a gateway to MySQL
-- The External Table definition stores metadata about the external source
-- but does not retrieve any data until queried
CREATE EXTERNAL TABLE public.epeople (
    id INTEGER,
    name VARCHAR(20)
) AS COPY WITH
    SOURCE ODBCSource()
    PARSER ODBCLoader(
        connect='DSN=MySQL',
        query='SELECT * FROM testdb.people'
);

-- Test 1: Query with predicate pushdown
-- When executing a query with a WHERE clause, ODBCLoader rewrites the original
-- external query to include the predicate, pushing the filter to the external database
-- This minimizes the amount of data transferred from MySQL to Vertica
-- Expected: Only people with id > 100 are retrieved from MySQL
SELECT * FROM public.epeople WHERE id > 100;

-- Test 2: Query with column pruning
-- When only specific columns are selected, ODBCLoader optimizes the external source query
-- to fetch only the required columns, reducing data transfer from the remote database.
-- Unselected columns are not fetched and do not appear in the query result.
-- Expected: Only id column is fetched from MySQL (name column is not selected, so it is not fetched)
SELECT id FROM public.epeople WHERE id > 100;

-- Test 3: Federated join query
-- Demonstrates joining an external table (MySQL) with a Vertica-managed table
-- This leverages both databases' query engines for optimal performance
-- Create a temporary Vertica table for joining
CREATE TABLE public.employee_status (
    id INTEGER,
    status VARCHAR(20)
);

INSERT INTO public.employee_status VALUES 
    (1, 'active'),
    (101, 'inactive'),
    (102, 'active'),
    (103, 'inactive');

-- Perform a federated join combining data from MySQL (epeople) and Vertica (employee_status)
-- The ODBCLoader will push down predicates to MySQL when possible
SELECT epeople.id, epeople.name, employee_status.status
FROM public.epeople 
JOIN public.employee_status ON epeople.id = employee_status.id
WHERE epeople.id > 100;

-- Test 4: Complex predicate pushdown
-- Multiple predicates are combined and pushed to the external database
-- Expected: MySQL executes: SELECT * FROM testdb.people WHERE id > 50 AND id < 150
SELECT id, name FROM public.epeople WHERE id > 50 AND id < 150;

-- Clean up external and temporary tables
DROP TABLE public.epeople;
DROP TABLE public.employee_status;
