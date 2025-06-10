# Ensure that the view pg_stat_multixact accurately reflects the
# number of multixact members currently in use.
setup
{
  CREATE TABLE pg_stat_multixact_count_check (
	value int
  );

  CREATE TABLE pg_stat_multixact_count_state (
	session int,
	member_count int
  );

  INSERT INTO pg_stat_multixact_count_check VALUES (1);
}

teardown
{
  DROP TABLE pg_stat_multixact_count_check;
  DROP TABLE pg_stat_multixact_count_state;
}

session s1

setup
{
	BEGIN;
}

step s1_stat
{
	INSERT INTO pg_stat_multixact_count_state VALUES (1, (SELECT members FROM pg_stat_multixact));
}

step s1_lock
{
	SELECT * FROM pg_stat_multixact_count_check FOR SHARE;
}

step s1_commit
{
	COMMIT;
}

session s2

setup
{
	BEGIN;
}

step s2_lock
{
	SELECT * FROM pg_stat_multixact_count_check FOR SHARE;
}

step s2_stat
{
	INSERT INTO pg_stat_multixact_count_state VALUES (2, (SELECT members FROM pg_stat_multixact));
}

step s2_commit
{
	COMMIT;
}

session s3

setup
{
	BEGIN;
}

step s3_state
{
	WITH session_1_count AS (
		SELECT member_count FROM pg_stat_multixact_count_state WHERE session = 1
	),
	session_2_count AS (
		SELECT member_count FROM pg_stat_multixact_count_state WHERE session = 2
	)
	SELECT s2.member_count - s1.member_count AS diff
	FROM session_1_count s1
	CROSS JOIN session_2_count s2;
}

step s3_commit
{
	COMMIT;
}

permutation s1_stat s1_lock s2_lock s2_stat s1_commit s2_commit s3_state s3_commit
