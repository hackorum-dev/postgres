# Test that heap_update() keeps the locks that other transactions take on
# the old tuple while it has released the buffer lock.
#
# When the new tuple needs toasting, or doesn't fit on the old tuple's page,
# heap_update() marks the old tuple as locked by its own transaction and
# releases the buffer lock.  For an update that doesn't change key columns,
# the lock is FOR NO KEY UPDATE, so another transaction can still take a FOR
# KEY SHARE lock on the tuple, for example a foreign key check.  That lock
# must carry over to the old and the new tuple version once the update is
# done.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE t (id int PRIMARY KEY, val text);
	INSERT INTO t VALUES (1, 'initial');
	CREATE TABLE c (pid int REFERENCES t (id));

	-- random, incompressible text well over TOAST_TUPLE_THRESHOLD
	CREATE FUNCTION bigstring() RETURNS text AS $$
	  SELECT string_agg(md5(i::text), '') FROM generate_series(1, 200) i
	$$ LANGUAGE SQL;

	-- A table where the tuple with id = 1 is on a full page, so that
	-- updating it needs a new page, without toasting.
	CREATE TABLE f (id int PRIMARY KEY, other int);
	DO $$
		DECLARE
			i int;
			tid tid;
		BEGIN
			FOR i IN 1..5000 LOOP
				INSERT INTO f VALUES (i, 0) RETURNING ctid INTO tid;
				IF tid = '(1,1)' THEN
					RETURN;
				END IF;
			END LOOP;
			RAISE 'expected to insert tuple to (1,1)';
		END;
	$$;
}
teardown
{
	DROP TABLE c;
	DROP TABLE t;
	DROP TABLE f;
	DROP FUNCTION bigstring();
	DROP EXTENSION injection_points;
}

session s1
setup	{
	SELECT FROM injection_points_set_local();
	SELECT FROM injection_points_attach('heap_update-unlocked', 'wait');
}
step s1begin		{ BEGIN; }
step s1toast		{ UPDATE t SET val = bigstring() WHERE id = 1; }
step s1newpage		{ UPDATE f SET other = other + 1 WHERE id = 1; }
step s1rollback		{ ROLLBACK; }
step s1keyupdate	{ UPDATE t SET id = id + 1; }
step s1keyupdatef	{ UPDATE f SET id = -id WHERE id = 1; }
step s1delete		{ DELETE FROM t WHERE id = 1; }
step s1check		{ SELECT id, length(val) FROM t; }
step s1orphans		{ SELECT count(*) AS orphans FROM c WHERE pid NOT IN (SELECT id FROM t); }

session s2
step s2begin		{ BEGIN; }
step s2keyshare		{ SELECT id FROM t WHERE id = 1 FOR KEY SHARE; }
step s2keysharef	{ SELECT id FROM f WHERE id = 1 FOR KEY SHARE; }
step s2insertchild	{ INSERT INTO c VALUES (1); }
step s2commit		{ COMMIT; }

session s3
step s3begin		{ BEGIN; }
step s3keyshare		{ SELECT id FROM t WHERE id = 1 FOR KEY SHARE; }
step s3commit		{ COMMIT; }

session wake
step wake	{
	SELECT FROM injection_points_detach('heap_update-unlocked');
	SELECT FROM injection_points_wakeup('heap_update-unlocked');
}

# s2 takes a FOR KEY SHARE lock while s1 is toasting.  A key update of the
# new tuple version must wait for s2.
permutation
	s1toast
	s2begin s2keyshare
	wake
	s1keyupdate
	s2commit
	s1check

# Same, but the update needs a new page instead of toasting.
permutation
	s1newpage
	s2begin s2keysharef
	wake
	s1keyupdatef
	s2commit

# A foreign key check from s2 inserting a child row.  Deleting the parent
# must wait for s2 and then fail, leaving no orphaned child row.
permutation
	s1toast
	s2begin s2insertchild
	wake
	s1delete
	s2commit
	s1orphans

# If the update rolls back, the old tuple version must still be locked.
permutation
	s1begin s1toast
	s2begin s2keyshare
	wake
	s1rollback
	s1keyupdate
	s2commit
	s1check

# Two lockers in the window; the key update must wait for both.
permutation
	s1toast
	s2begin s2keyshare
	s3begin s3keyshare
	wake
	s1keyupdate
	s2commit
	s3commit
	s1check

# A locker that ends while s1 is toasting doesn't hold anything after that.
permutation
	s1toast
	s2begin s2keyshare s2commit
	wake
	s1keyupdate
	s1check
