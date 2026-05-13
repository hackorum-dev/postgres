# Test vacuum_freeze_terminate_blockers_pid.
#
# A transaction with an old XID can hold back VACUUM's freeze cutoff.  Once
# table age passes the freeze termination age derived from vacuum_failsafe_age
# and autovacuum_freeze_score_weight, enabling the GUC should make VACUUM
# terminate the backend that owns that blocker XID.

setup
{
	CREATE TABLE vacuum_freeze_blocker_tab (id int)
		WITH (autovacuum_enabled = off);
	INSERT INTO vacuum_freeze_blocker_tab VALUES (1);
	CREATE TABLE vacuum_freeze_blocker_pid (pid int);
	CREATE TABLE vacuum_freeze_xid_burner (id int);
}

# Unsafe xid_wraparound tests can consume billions of XIDs.  This default
# isolation test keeps runtime practical by using a small vacuum_failsafe_age
# and burning enough XIDs to cross the same age threshold formula.
# Each setup block runs separately.
setup { INSERT INTO vacuum_freeze_xid_burner DEFAULT VALUES; }
setup { INSERT INTO vacuum_freeze_xid_burner DEFAULT VALUES; }
setup { INSERT INTO vacuum_freeze_xid_burner DEFAULT VALUES; }
setup { INSERT INTO vacuum_freeze_xid_burner DEFAULT VALUES; }
setup { INSERT INTO vacuum_freeze_xid_burner DEFAULT VALUES; }
setup { INSERT INTO vacuum_freeze_xid_burner DEFAULT VALUES; }

teardown
{
	DROP TABLE IF EXISTS vacuum_freeze_blocker_tab;
	DROP TABLE IF EXISTS vacuum_freeze_blocker_pid;
	DROP TABLE IF EXISTS vacuum_freeze_xid_burner;
}

session blocker
step blocker_record_pid
{
	INSERT INTO vacuum_freeze_blocker_pid SELECT pg_backend_pid();
}
step blocker_begin
{
	BEGIN;
}
step blocker_assign_xid
{
	SELECT txid_current() IS NOT NULL AS xid_assigned;
}

session vacuumer
setup
{
	SET client_min_messages = error;
	SET vacuum_failsafe_age = 6;
	SET vacuum_freeze_min_age = 0;
	SET vacuum_freeze_terminate_blockers_pid = on;
}
step vacuum_run
{
	VACUUM vacuum_freeze_blocker_tab;
}
step vacuum_check_age_past_threshold
{
	SELECT age(relfrozenxid)::float8 >
		CASE WHEN current_setting('autovacuum_freeze_score_weight')::float8 > 0.0
			THEN current_setting('vacuum_failsafe_age')::float8 /
				current_setting('autovacuum_freeze_score_weight')::float8
			ELSE current_setting('vacuum_failsafe_age')::float8
		END AS past_termination_age
	FROM pg_class
	WHERE oid = 'vacuum_freeze_blocker_tab'::regclass;
}
step vacuum_check_blocker_gone
{
	SELECT count(*) = 0 AS blocker_gone
	FROM pg_stat_activity
	WHERE pid = (SELECT pid FROM vacuum_freeze_blocker_pid);
}

permutation
	blocker_record_pid
	blocker_begin
	blocker_assign_xid
	vacuum_check_age_past_threshold
	vacuum_run
	vacuum_check_blocker_gone
