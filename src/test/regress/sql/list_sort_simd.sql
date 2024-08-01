--
-- Tests for list sort
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_list_sort_simd_float (arg1 integer, arg2 integer)
        RETURNS float[]
        AS :'regresslib'
        LANGUAGE C STRICT;

CREATE FUNCTION test_list_sort_simd_float_random (arg1 integer, arg2 integer, arg3 integer, arg4 boolean)
        RETURNS float[]
        AS :'regresslib'
        LANGUAGE C STRICT;

SELECT test_list_sort_simd_float(100, 10);
SELECT test_list_sort_simd_float(10, 5);
SELECT test_list_sort_simd_float_random(100, 20, 2, true);
SELECT test_list_sort_simd_float_random(2, 20, 2, true);
SELECT test_list_sort_simd_float_random(1000, 200, 20, true);
SELECT test_list_sort_simd_float_random(10000, 20, 2, true);
SELECT test_list_sort_simd_float_random(10000, 200, 50, true);
SELECT test_list_sort_simd_float_random(10000, 2000, 500, true);
SELECT test_list_sort_simd_float_random(100000, 20000, 5000, true);
SELECT test_list_sort_simd_float_random(100000, 20000, 1000, true);
SELECT test_list_sort_simd_float_random(100, 20, 2, false);
SELECT test_list_sort_simd_float_random(10000, 3000, 300, false);