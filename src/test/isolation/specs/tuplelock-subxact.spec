# Here we verify that XactLockTableWait() works with subxacts

setup {
	DROP TABLE IF EXISTS subxid_conflict;
	CREATE TABLE subxid_conflict (a int primary key, b int);
	INSERT INTO subxid_conflict VALUES (1, 0), (2, 0), (3, 0);
}

teardown {
	DROP TABLE subxid_conflict;
}

session s1
step s1_b  { BEGIN; }
step s1_u1 { 				UPDATE subxid_conflict SET b=1 WHERE a=1; }
step s1_u2 { SAVEPOINT s1;	UPDATE subxid_conflict SET b=1 WHERE a=2; }
step s1_u3 { SAVEPOINT s2;	UPDATE subxid_conflict SET b=1 WHERE a=3; }
step s1_subcommit2  { RELEASE SAVEPOINT s2; }
# waiter should now wait for parent
step s1_abort1  { ROLLBACK TO s1; }
step s1_abort2  { ROLLBACK TO s2; }
# waiters should now be released
step s1_c { COMMIT; }
step s1_s { SELECT a, b FROM subxid_conflict WHERE a = 3; }

session s2
step s2_u3 { BEGIN;			UPDATE subxid_conflict SET b=2 WHERE a=3; COMMIT; }

session s3
step s3_u3 { BEGIN;			UPDATE subxid_conflict SET b=3 WHERE a=3; COMMIT; }

# result should be (1, 1), (2, 1), (3, 3)
permutation s1_b s1_u1 s1_u2 s1_u3 s2_u3 s1_subcommit2 s1_abort1 s3_u3 s1_c s1_s
permutation s1_b s1_u1 s1_u2 s1_u3 s2_u3               s1_abort2 s3_u3 s1_c s1_s
