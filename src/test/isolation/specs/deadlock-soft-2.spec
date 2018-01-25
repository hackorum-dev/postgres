# Soft deadlock requiring reversal of multiple wait-edges.  s1 must
# jump over both s3 and s4 and acquire the lock on aa2 immediately,
# since s3 and s4 are hard-blocked on aa1.

# The expected output for this test assumes that isolationtester will
# detect step s1b as waiting before the deadlock detector runs and
# releases s1 from its blocked state.  To leave enough time for that
# to happen even in very slow (CLOBBER_CACHE_ALWAYS) cases, we must
# increase deadlock_timeout.

setup
{
  CREATE TABLE aa1 ();
  CREATE TABLE aa2 ();
}

teardown
{
  DROP TABLE aa1, aa2;
}

session "s1"
setup		{ BEGIN; SET deadlock_timeout = '5s'; }
step "s1a"	{ LOCK TABLE aa1 IN SHARE UPDATE EXCLUSIVE MODE; }
step "s1b"	{ LOCK TABLE aa2 IN SHARE UPDATE EXCLUSIVE MODE; }
step "s1c"	{ COMMIT; }

session "s2"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s2a" 	{ LOCK TABLE aa2 IN ACCESS SHARE MODE; }
step "s2b" 	{ LOCK TABLE aa1 IN SHARE UPDATE EXCLUSIVE MODE; }
step "s2c"	{ COMMIT; }

session "s3"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s3a"	{ LOCK TABLE aa2 IN ACCESS EXCLUSIVE MODE; }
step "s3c"	{ COMMIT; }

session "s4"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s4a"	{ LOCK TABLE aa2 IN ACCESS EXCLUSIVE MODE; }
step "s4c"	{ COMMIT; }

permutation "s1a" "s2a" "s2b" "s3a" "s4a" "s1b" "s1c" "s2c" "s3c" "s4c"
