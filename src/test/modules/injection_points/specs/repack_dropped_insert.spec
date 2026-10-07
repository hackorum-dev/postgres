# Test REPACK (CONCURRENTLY) replaying an INSERT that carries the value of a
# dropped column.
#
# The trigger makes the new tuple a copy of tuple 1, dropped column included.
# The columns that are left are ints, so the new heap has no TOAST table to
# put that value in.
setup {
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_dropped_insert (id int PRIMARY KEY, n int, b text);
	ALTER TABLE repack_dropped_insert ALTER COLUMN b SET STORAGE EXTERNAL;
	INSERT INTO repack_dropped_insert (id, n, b) VALUES (1, 1,
		repeat('x', 2 * current_setting('block_size')::int));
	CREATE FUNCTION repack_dropped_insert_f() RETURNS trigger LANGUAGE plpgsql AS
		$$ DECLARE r repack_dropped_insert;
		BEGIN
			r := (SELECT t FROM repack_dropped_insert t WHERE id = 1);
			r.id := NEW.id;
			RETURN r;
		END $$;
	CREATE TRIGGER repack_dropped_insert_t BEFORE INSERT ON repack_dropped_insert FOR EACH ROW EXECUTE FUNCTION repack_dropped_insert_f();
	ALTER TABLE repack_dropped_insert DROP COLUMN b;
}

teardown {
	DROP TABLE repack_dropped_insert;
	DROP FUNCTION repack_dropped_insert_f;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
step s1_repack
{
	REPACK (CONCURRENTLY) repack_dropped_insert;
}
step s1_noop { }

session s2
step s2_insert
{
	INSERT INTO repack_dropped_insert (id, n) VALUES (2, 2);
}
step s2_size
{
	SELECT id, pg_column_size(repack_dropped_insert) < current_setting('block_size')::int
		AS fits_in_one_block
		FROM repack_dropped_insert ORDER BY id;
}
step s2_unlock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation
	s1_repack
	s2_insert
	s2_size
	s2_unlock
	s1_noop
	s2_size
