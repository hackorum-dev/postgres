# Key-join revalidation must tolerate a concurrently dropped dependent policy.
#
# Revalidation finds the policy in pg_depend while a DROP POLICY is still
# uncommitted, then waits on the policy's lock.  Once the drop commits, the
# stored proof is gone and the triggering DDL must not dereference the stale
# policy OID.

setup
{
 CREATE SCHEMA key_join_stale_policy_dependency;
 CREATE TABLE key_join_stale_policy_dependency.p
 (
     id int PRIMARY KEY
 );
 CREATE TABLE key_join_stale_policy_dependency.c
 (
     id int PRIMARY KEY,
     parent_id int CONSTRAINT c_parent_id_not_null NOT NULL,
     CONSTRAINT c_parent_id_fkey
         FOREIGN KEY (parent_id)
         REFERENCES key_join_stale_policy_dependency.p (id)
 );
 CREATE TABLE key_join_stale_policy_dependency.guarded
 (
     id int
 );
 INSERT INTO key_join_stale_policy_dependency.p VALUES (1);
 INSERT INTO key_join_stale_policy_dependency.c VALUES (10, 1);
 ALTER TABLE key_join_stale_policy_dependency.guarded
     ENABLE ROW LEVEL SECURITY;
 CREATE POLICY pol ON key_join_stale_policy_dependency.guarded
 USING (EXISTS (
     SELECT 1
     FROM key_join_stale_policy_dependency.p p
     JOIN key_join_stale_policy_dependency.c c
         FOR KEY p (id) <- c (parent_id)
 ));
}

teardown
{
 DROP SCHEMA key_join_stale_policy_dependency CASCADE;
}

session s1
step s1_drop_policy
{
 BEGIN;
 DROP POLICY pol ON key_join_stale_policy_dependency.guarded;
}
step s1_commit { COMMIT; }

session s2
step s2_alter_fk
{
 ALTER TABLE key_join_stale_policy_dependency.c
     ALTER CONSTRAINT c_parent_id_fkey NOT ENFORCED;
}

permutation s1_drop_policy s2_alter_fk s1_commit
