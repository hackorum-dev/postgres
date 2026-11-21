# Test inline permutation steps from deadlock-hard.spec

setup
{
  CREATE TABLE a1 ();
  CREATE TABLE a2 ();
  CREATE TABLE a3 ();
  CREATE TABLE a4 ();
  CREATE TABLE a5 ();
  CREATE TABLE a6 ();
  CREATE TABLE a7 ();
  CREATE TABLE a8 ();
}

teardown
{
  DROP TABLE a1, a2, a3, a4, a5, a6, a7, a8;
}

session s1
setup		{ BEGIN; SET deadlock_timeout = '100s'; }

session s2
setup		{ BEGIN; SET deadlock_timeout = '100s'; }

session s3
setup		{ BEGIN; SET deadlock_timeout = '100s'; }

session s4
setup		{ BEGIN; SET deadlock_timeout = '100s'; }

session s5
setup		{ BEGIN; SET deadlock_timeout = '100s'; }

session s6
setup		{ BEGIN; SET deadlock_timeout = '100s'; }

session s7
setup		{ BEGIN; SET deadlock_timeout = '100s'; }

session s8
setup		{ BEGIN; SET deadlock_timeout = '10ms'; }

# Note: when s8[2] detects the deadlock and fails, s7[2] is released, making
# it timing-dependent which query completion is received first by the tester.
# To ensure output stability, add a blocking mark to force s8[2]'s completion
# to be reported first.  There is a second timing dependency, too: the tester
# might or might not observe s8[2] during its short lock wait state.  Apply a
# dummy blocking mark to s8[2] to ensure it will be reported as "waiting"
# regardless of that.

permutation
  s1 { LOCK TABLE a1; }
  s2 { LOCK TABLE a2; }
  s3 { LOCK TABLE a3; }
  s4 { LOCK TABLE a4; }
  s5 { LOCK TABLE a5; }
  s6 { LOCK TABLE a6; }
  s7 { LOCK TABLE a7; }
  s8 { LOCK TABLE a8; }
  s1 { LOCK TABLE a2; }
  s2 { LOCK TABLE a3; }
  s3 { LOCK TABLE a4; }
  s4 { LOCK TABLE a5; }
  s5 { LOCK TABLE a6; }
  s6 { LOCK TABLE a7; }
  s7("s8[2]") { LOCK TABLE a8; }
  s8(*) { LOCK TABLE a1; }
  s8 { COMMIT; }
  s7 { COMMIT; }
  s6 { COMMIT; }
  s5 { COMMIT; }
  s4 { COMMIT; }
  s3 { COMMIT; }
  s2 { COMMIT; }
  s1 { COMMIT; }
