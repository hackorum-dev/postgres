# ALTER TABLE - SET NOT NULL with an older snapshot
#
# SET NOT NULL validates the column by scanning only the rows visible to its
# own snapshot, and does not rewrite the table.  A transaction holding an
# older snapshot can therefore still see a row whose column is null, and must
# continue to read that row correctly.

setup
{
 CREATE TABLE nn (a int, b int, c int);
 INSERT INTO nn VALUES (NULL, 42, 43);
}

teardown
{
 DROP TABLE nn;
}

session s1
setup		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1snap	{ SELECT 1 AS snapshot_taken; }
step s1read	{ SELECT a, b, c FROM nn; }
step s1c	{ COMMIT; }

session s2
step s2del	{ DELETE FROM nn WHERE a IS NULL; }
step s2nn	{ ALTER TABLE nn ALTER COLUMN a SET NOT NULL; }

permutation s1snap s2del s2nn s1read s1c
