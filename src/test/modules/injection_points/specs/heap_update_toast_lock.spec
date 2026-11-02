# Test that heap_update() protects the old tuple from concurrent updates
# while it has released the buffer lock to do TOASTing.
#
# When the new tuple needs toasting (or doesn't fit on the page), heap_update()
# temporarily marks the old tuple as locked by its own transaction, using the
# tuple lock mode computed from the modified columns (LockTupleNoKeyExclusive
# if no key columns were changed), and releases the buffer lock.  While it's
# not holding the buffer lock, another transaction can acquire a KEY SHARE lock
# on the same tuple.
#
# The bug: the FOR SHARE lock acquired in that window will be lost when the
# update finishes, because the update overwrites the tuple's xmax with a new
# xmax and infomasks that were computed before releasing the lock.
setup
{
        CREATE EXTENSION injection_points;

        CREATE TABLE t (id int PRIMARY KEY, val text, other int);
        INSERT INTO t VALUES (1, 'initial', 0);

	-- random, incompressible text well over TOAST_TUPLE_THRESHOLD
	CREATE FUNCTION bigstring() RETURNS text AS $$
	  SELECT string_agg(md5(i::text), '') FROM generate_series(1, 200) i
	$$ LANGUAGE SQL
}
teardown
{
        DROP TABLE t;
        DROP EXTENSION injection_points;
}

session s1
setup   {
        SELECT FROM injection_points_set_local();
        SELECT FROM injection_points_attach('heap_update-toast-unlocked', 'wait');
}

step s1update     { UPDATE t SET val = bigstring() WHERE id = 1; }
step s1keyupdate  { UPDATE t SET id = id + 1;}

step s1check      { SELECT id, length(val), other FROM t; }

session s2
step s2begin      { BEGIN; }
step s2keyshare   { SELECT id FROM t WHERE id = 1 FOR KEY SHARE; }
step s2commit     { COMMIT; }

session wake
step wake       {
        SELECT FROM injection_points_detach('heap_update-toast-unlocked');
        SELECT FROM injection_points_wakeup('heap_update-toast-unlocked');
}

# Concurrent non-key update must wait for s1 to commit.
permutation
    # Start UPDATE on the tuple. This pauses on the injection point, after
    # locking the tuple but before completing the UPDATE.
    s1update

    # Lock the tuple from another session
    s2begin s2keyshare

    # Finish the update and the s2 locker. Session s2 continues to
    # hold its KEY SHARE lock.
    wake(s1update)

    # Try to update the key column. This should block on s2's FOR KEY SHARE lock
    #
    # FIXME: this does not block as it should
    s1keyupdate

    s2commit
    s1check
