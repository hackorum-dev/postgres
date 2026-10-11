# Test find_all_inheritors() with NoLock while a partitioned table below
# the first level is dropped concurrently.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE root (a int) PARTITION BY LIST (a);
	CREATE TABLE p1 PARTITION OF root FOR VALUES IN (1, 2) PARTITION BY LIST (a);
	-- q is expanded before p2, its pg_inherits scan doesn't see p2 anymore
	CREATE TABLE q PARTITION OF p1 FOR VALUES IN (1) PARTITION BY LIST (a);
	CREATE TABLE q1 PARTITION OF q FOR VALUES IN (1);
	CREATE TABLE p2 PARTITION OF p1 FOR VALUES IN (2) PARTITION BY LIST (a);
	CREATE TABLE p21 PARTITION OF p2 FOR VALUES IN (2);
}

teardown
{
	DROP TABLE root;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('find-inheritance-children-before-scan',
								   'wait', 'q'::regclass::oid::text);
}
step s1_vacuum	{ VACUUM root; }

session s2
step s2_drop	{ DROP TABLE p2; }
step s2_wakeup
{
	SELECT injection_points_detach('find-inheritance-children-before-scan');
	SELECT injection_points_wakeup('find-inheritance-children-before-scan');
}

permutation s1_vacuum s2_drop s2_wakeup
