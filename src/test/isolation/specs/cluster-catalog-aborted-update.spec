# VACUUM FULL on a catalog, with an aborted update by a concurrent
# transaction
#
# s1 creates a table and then updates its pg_class row in a subtransaction
# that rolls back.  s1 is still open when s2 rewrites pg_class, and no one
# has set HEAP_XMAX_INVALID on the pg_class row yet.  The row must survive.

teardown
{
  DROP TABLE IF EXISTS cluster_catalog_tab;
}

session s1
step s1_create	{ BEGIN; CREATE TABLE cluster_catalog_tab (a int); }
step s1_alter	{ SAVEPOINT sp; ALTER TABLE cluster_catalog_tab ADD COLUMN b int; ROLLBACK TO sp; }
step s1_commit	{ COMMIT; }

session s2
step s2_vacfull	{ VACUUM FULL pg_class; }
step s2_check	{ SELECT count(*) FROM pg_class WHERE relname = 'cluster_catalog_tab'; }

permutation s1_create s1_alter s2_vacfull s1_commit s2_check
