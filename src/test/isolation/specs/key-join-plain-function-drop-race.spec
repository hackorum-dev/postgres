# A function definition change that waited for another one must notice that
# the function was dropped in the meantime.
#
# Session s1 alters a function and keeps the transaction open, so that
# CREATE OR REPLACE FUNCTION or ALTER FUNCTION in s2 waits.  s1 then drops
# the function and commits; s2 must fail cleanly.

setup
{
 CREATE SCHEMA key_join_plain_function_drop_race;
 CREATE FUNCTION key_join_plain_function_drop_race.g() RETURNS int
     LANGUAGE sql STABLE AS 'SELECT 1';
 CREATE FUNCTION key_join_plain_function_drop_race.h() RETURNS int
     LANGUAGE sql STABLE AS 'SELECT 1';
}

teardown
{
 DROP SCHEMA key_join_plain_function_drop_race CASCADE;
}

session s1
step s1_begin { BEGIN; }
step s1_lock_g { ALTER FUNCTION key_join_plain_function_drop_race.g() COST 1; }
step s1_drop_g { DROP FUNCTION key_join_plain_function_drop_race.g(); }
step s1_lock_h { ALTER FUNCTION key_join_plain_function_drop_race.h() COST 1; }
step s1_drop_h { DROP FUNCTION key_join_plain_function_drop_race.h(); }
step s1_commit { COMMIT; }

session s2
step s2_replace_g
{
 CREATE OR REPLACE FUNCTION key_join_plain_function_drop_race.g() RETURNS int
     LANGUAGE sql STABLE AS 'SELECT 2';
}
step s2_alter_h { ALTER FUNCTION key_join_plain_function_drop_race.h() COST 2; }

permutation s1_begin s1_lock_g s2_replace_g s1_drop_g s1_commit
permutation s1_begin s1_lock_h s2_alter_h s1_drop_h s1_commit
