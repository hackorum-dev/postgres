# MultiXactIdExpand must not reuse a multixact that still contains a member
# which is no longer of interest.
#
# s1 locks the row FOR NO KEY UPDATE, updates it in a subtransaction, rolls
# that subtransaction back, and releases the savepoint.  The next statement
# therefore runs as s1 itself.  Its xid is already a member of the row's
# xmax, with status ForNoKeyUpdate.  The second update changes val, which is
# not a key column, so the temporary lock in heap_update uses that same
# status.  The new value is toasted, so heap_update takes that temporary
# lock.  The xmax computed there has to be lock-only.  An aborted updater
# left in the multixact used to leave HEAP_XMAX_LOCK_ONLY unset, and an
# assertion build failed in Assert(HEAP_XMAX_IS_LOCKED_ONLY).
#
# Updating id instead of val would take ForUpdate for the temporary lock.
# That status does not match, and the old code already pruned.

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
