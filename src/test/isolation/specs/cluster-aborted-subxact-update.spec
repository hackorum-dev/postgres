# VACUUM FULL must keep a row whose inserting transaction is still running,
# when a rolled-back subtransaction of that transaction updated the row.
#
# VACUUM FULL of a user table would wait for the inserter, so this uses
# pg_class.  The inserter creates a table and adds a column in a subtransaction
# that it rolls back, which leaves an updated pg_class row whose updater
# aborted.

teardown
{
	DROP TABLE IF EXISTS vacfull_tab;
}

session creator
step create_and_rollback_alter
{
	BEGIN;
	CREATE TABLE vacfull_tab (a int);
	SAVEPOINT sp;
	ALTER TABLE vacfull_tab ADD COLUMN b int;
	ROLLBACK TO sp;
}
step commit	{ COMMIT; }

session vacuumer
step vacuum_full_pg_class	{ VACUUM FULL pg_class; }
step check_pg_class
{
	SELECT relname, relnatts FROM pg_class WHERE relname = 'vacfull_tab';
}

permutation create_and_rollback_alter vacuum_full_pg_class commit check_pg_class
