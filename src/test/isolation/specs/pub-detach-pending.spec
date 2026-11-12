# Pending detach commits inhdetachpending, then waits. An ALL TABLES
# publication with publish_via_partition_root must list that partition.

setup
{
	CREATE TABLE parent_tbl (id int) PARTITION BY RANGE (id);
	CREATE TABLE child_tbl PARTITION OF parent_tbl FOR VALUES FROM (0) TO (100);
	CREATE PUBLICATION pub FOR ALL TABLES WITH (publish_via_partition_root = true);
}

teardown
{
	DROP PUBLICATION pub;
	DROP TABLE parent_tbl, child_tbl;
}

session s1
step lock	{ BEGIN; LOCK TABLE parent_tbl IN ACCESS SHARE MODE; }
step unlock	{ COMMIT; }

session s2
step detach	{ ALTER TABLE parent_tbl DETACH PARTITION child_tbl CONCURRENTLY; }

session s3
step listed	{ SELECT tablename FROM pg_publication_tables WHERE pubname = 'pub' AND tablename = 'child_tbl'; }

permutation lock detach listed unlock
