# ADD COLUMN CONCURRENTLY
#
# Example from bug report 14691.

setup
{
	CREATE TABLE foo(x integer);
	INSERT INTO foo VALUES (1), (2), (3);
}

teardown
{
	DROP TABLE foo;
}

session "s1"
step "s1b" { BEGIN ISOLATION LEVEL READ COMMITTED; }
step "s1a" { ALTER TABLE foo ADD COLUMN y integer; }
step "s1u" { UPDATE foo SET y = 0; }
step "s1c" { COMMIT; }

session "s2"
step "s2b" { BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY DEFERRABLE; }
step "s2s" { TABLE foo; }
step "s2c" { COMMIT; }

session "s3"
step "s3b" { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "s3a" { ALTER TABLE foo ADD COLUMN y integer; }
step "s3u" { UPDATE foo SET y = 0; }
step "s3c" { COMMIT; }

permutation "s1b" "s1a" "s1u" "s2b" "s2s" "s1c" "s2c"
permutation "s3b" "s3a" "s3u" "s2b" "s2s" "s3c" "s2c"
