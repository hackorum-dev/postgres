# CREATE OR REPLACE FUNCTION checks ownership before and after waiting for a
# concurrent definition change.
#
# Session s1 alters f() and keeps the transaction open.  A session that does
# not own f() must fail the owner check immediately, without waiting for s1.
# The owner waits; if s1 gives f() away before committing, the waiter must
# fail the owner check afterwards.

setup
{
 CREATE ROLE key_join_plain_function_owner;
 CREATE ROLE key_join_plain_function_other;
 CREATE SCHEMA key_join_plain_function_ownercheck_lock
     AUTHORIZATION key_join_plain_function_owner;
 GRANT USAGE, CREATE ON SCHEMA key_join_plain_function_ownercheck_lock
     TO key_join_plain_function_other;
 SET ROLE key_join_plain_function_owner;
 CREATE FUNCTION key_join_plain_function_ownercheck_lock.f() RETURNS int
     LANGUAGE sql RETURN 1;
 RESET ROLE;
}

teardown
{
 DROP SCHEMA key_join_plain_function_ownercheck_lock CASCADE;
 DROP ROLE key_join_plain_function_owner;
 DROP ROLE key_join_plain_function_other;
}

session s1
step s1_begin { BEGIN; }
step s1_alter_f
{
 ALTER FUNCTION key_join_plain_function_ownercheck_lock.f() COST 11;
}
step s1_owner_f
{
 ALTER FUNCTION key_join_plain_function_ownercheck_lock.f()
 OWNER TO key_join_plain_function_other;
}
step s1_commit { COMMIT; }

session s2
step s2_auth { SET ROLE key_join_plain_function_other; }
step s2_auth_owner { SET ROLE key_join_plain_function_owner; }
step s2_replace_f
{
 CREATE OR REPLACE FUNCTION key_join_plain_function_ownercheck_lock.f()
 RETURNS int
 LANGUAGE sql
 RETURN 2;
}

permutation s1_begin s1_alter_f s2_auth s2_replace_f s1_commit
permutation s1_begin s1_alter_f s2_auth_owner s2_replace_f s1_owner_f s1_commit
