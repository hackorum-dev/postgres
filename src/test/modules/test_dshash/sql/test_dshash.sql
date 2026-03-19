CREATE EXTENSION test_dshash;

-- Exercise core dshash operations.
SELECT test_dshash_basic();

-- Exercise OOM handling: NO_OOM returns NULL, regular insert raises ERROR.
-- Use terse verbosity to ignore the DETAIL message.
\set VERBOSITY terse
SELECT test_dshash_find_or_insert_oom_error();
\set VERBOSITY default
