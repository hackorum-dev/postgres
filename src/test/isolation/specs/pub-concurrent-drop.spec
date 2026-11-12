# Tests for concurrently dropping a relation or the publication itself while a publication's tables are
# being listed.

setup
{
	CREATE SCHEMA pubdrop;
	CREATE PUBLICATION pub_schema FOR TABLES IN SCHEMA pubdrop;
	CREATE TABLE pubdrop.dropme (id int);
	CREATE TABLE pubdrop.keepme (id int);
}

teardown
{
	DROP SCHEMA pubdrop CASCADE;
	DROP PUBLICATION IF EXISTS pub_schema;
}

session s1
step lock	{ BEGIN; LOCK pubdrop.dropme IN ACCESS EXCLUSIVE MODE; }
step drop_and_commit	{ DROP TABLE pubdrop.dropme; COMMIT; }
step drop_pub_and_commit	{ DROP PUBLICATION pub_schema; COMMIT; }

session s2
step list_pub_tables
{
	SELECT relid::regclass AS tablename
	FROM pg_get_publication_tables('pub_schema')
	ORDER BY tablename;
}
step list_pub_table
{
	SELECT relid::regclass AS tablename
	FROM pg_get_publication_tables(ARRAY['pub_schema'], 'pubdrop.dropme'::regclass);
}

# Hold an ACCESS EXCLUSIVE lock on the table in one session, so that the query
# listing a publication's tables in another session blocks when it tries to
# open the locked table. Then drop the table in the same lock-holding session
# and commit, releasing the lock, so the query in another session resumes and
# skips the now-dropped table instead of erroring with "could not open relation
# with OID".
permutation lock list_pub_tables drop_and_commit

# Same as above, but drop the publication instead of the table. When the
# query resumes, the publication it collected the table OIDs from is gone.
# Rather than returning a partial list, it reports that the publication was
# concurrently dropped instead of failing with "cache lookup failed for
# publication".
permutation lock list_pub_tables drop_pub_and_commit

# The variant that takes a table OID ignores missing publications, so it
# skips the row for the dropped publication instead.
permutation lock list_pub_table drop_pub_and_commit
