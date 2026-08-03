-- src/test/modules/test_parallel_dml_safety/test_parallel_dml_safety--1.0.sql

CREATE FUNCTION test_parallel_dml_safety(regclass)
RETURNS "char"
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT PARALLEL UNSAFE;
