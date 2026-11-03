# Test concurrently dropping a partition while the partition tree of a
# partitioned table is being listed without locking the partitions.
#
# VACUUM and LOCK TABLE list the partitions with find_all_inheritors(NoLock),
# as the callers of that function may do: they do not want to lock the
# partitions yet.  Dropping a partition locks only the partition and its direct
# parent, so a partition that is not a direct child of the table can be dropped
# while the list is being built.  The command must not fail because of that:
# the partition is just not processed.
#
#   root
#   |---- p1
#   |      +---- trigger
#   |             +---- leaf
#   +---- p2
#          +---- victim
#
# s1 stops in find_all_inheritors() just before it looks for the children of
# "trigger", when "victim" has already been found.  s2 drops "victim", which
# does not conflict with s1.  When s1 goes on, scanning the children of a
# partitioned table has refreshed its caches, so the lookup of "victim" no
# longer finds it.
#
# The steps are plain commands: they are expected to complete.  A failure would
# print "cache lookup failed for relation <oid>".

setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE root (a int, b int) PARTITION BY RANGE (a);
	CREATE TABLE p1 PARTITION OF root FOR VALUES FROM (0) TO (10)
		PARTITION BY RANGE (b);
	CREATE TABLE p2 PARTITION OF root FOR VALUES FROM (10) TO (20)
		PARTITION BY RANGE (b);
	CREATE TABLE trigger PARTITION OF p1 FOR VALUES FROM (0) TO (10)
		PARTITION BY HASH (a);
	CREATE TABLE leaf PARTITION OF trigger
		FOR VALUES WITH (MODULUS 1, REMAINDER 0);
	CREATE TABLE victim PARTITION OF p2 FOR VALUES FROM (0) TO (10);
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
	SELECT injection_points_attach(
		'find-all-inheritors-before-scan',
		'wait',
		'trigger'::regclass::oid::text);
}

step vacuum_root	{ VACUUM root; }
step lock_root		{ BEGIN; LOCK TABLE root IN ACCESS SHARE MODE; }
step commit			{ COMMIT; }
# The session lives through both permutations, so detach the point.
teardown
{
	SELECT injection_points_detach('find-all-inheritors-before-scan');
}

session s2
step drop_victim	{ DROP TABLE victim; }
step wakeup
{
	SELECT injection_points_wakeup('find-all-inheritors-before-scan');
}

permutation vacuum_root drop_victim wakeup
permutation lock_root drop_victim wakeup commit
