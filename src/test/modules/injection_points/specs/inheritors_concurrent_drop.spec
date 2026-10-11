# Test find_all_inheritors() with NoLock while a partition below the first
# level is dropped concurrently.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE root (a int) PARTITION BY LIST (a);
	CREATE TABLE p1 PARTITION OF root FOR VALUES IN (1, 2) PARTITION BY LIST (a);
	CREATE TABLE a PARTITION OF p1 FOR VALUES IN (1) PARTITION BY LIST (a);
	CREATE TABLE a1 PARTITION OF a FOR VALUES IN (1);
	CREATE TABLE b PARTITION OF p1 FOR VALUES IN (2);
	CREATE PUBLICATION pub FOR TABLE root;
}

teardown
{
	DROP PUBLICATION pub;
	DROP TABLE root;
	DROP EXTENSION injection_points;
}

# Wait before expanding a, after p1's children a and b were collected.  b is
# dropped meanwhile, and the pg_inherits scan for a takes a new catalog
# snapshot, so the pg_class lookup for b doesn't find it anymore.
session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('find-all-inheritors-expand', 'wait',
								   'a'::regclass::oid::text);
}
step s1_vacuum	{ VACUUM root; }
step s1_pubtables
{
	SELECT tablename FROM pg_publication_tables WHERE pubname = 'pub'
	ORDER BY tablename;
}

session s2
step s2_drop	{ DROP TABLE b; }
step s2_wakeup
{
	SELECT injection_points_detach('find-all-inheritors-expand');
	SELECT injection_points_wakeup('find-all-inheritors-expand');
}

permutation s1_vacuum s2_drop s2_wakeup
permutation s1_pubtables s2_drop s2_wakeup
