# This test verifies that MultiXactIdExpand does not hand back a multixact
# that still contains members which are of no interest anymore.  A
# transaction that locked a row FOR NO KEY UPDATE updates it in a
# subtransaction which is then rolled back and released, and updates the row
# again by itself, with a new version that needs TOAST, so that heap_update
# temporarily locks the old tuple: the multixact it computes there must be
# locked-only, which an aborted update kept in it used to violate, failing
# Assert(HEAP_XMAX_IS_LOCKED_ONLY) in an assertion-enabled build.

setup
{
  DROP TABLE IF EXISTS multixact_expand;
  CREATE TABLE multixact_expand (id int primary key, val text);
  INSERT INTO multixact_expand VALUES (1, 'x');
}

teardown
{
  DROP TABLE multixact_expand;
}

session s1
step s1b	{ BEGIN; }
step s1l	{ SELECT id FROM multixact_expand WHERE id = 1 FOR NO KEY UPDATE; }
step s1sv	{ SAVEPOINT s; }
step s1u	{ UPDATE multixact_expand SET val = 'sub' WHERE id = 1; }
step s1rbs	{ ROLLBACK TO SAVEPOINT s; }
step s1rel	{ RELEASE SAVEPOINT s; }
step s1u2	{ UPDATE multixact_expand SET val = repeat('md5', 4000) WHERE id = 1; }
step s1len	{ SELECT length(val) FROM multixact_expand WHERE id = 1; }
step s1c	{ COMMIT; }

session s2
step s2sel	{ SELECT id FROM multixact_expand; }

permutation s1b s1l s1sv s1u s1rbs s1rel s1u2 s1len s1c s2sel
