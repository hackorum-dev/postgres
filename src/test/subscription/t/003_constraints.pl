
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# This test checks that constraints work on subscriber
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Setup structure on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_fk (bid int PRIMARY KEY);");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_fk_ref (id int PRIMARY KEY, junk text, bid int REFERENCES tab_fk (bid));"
);

# Setup structure on subscriber; column order intentionally different
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_fk (bid int PRIMARY KEY);");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_fk_ref (id int PRIMARY KEY, bid int REFERENCES tab_fk (bid), junk text);"
);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR ALL TABLES;");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub WITH (copy_data = false)"
);

$node_publisher->wait_for_catchup('tap_sub');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk (bid) VALUES (1);");
# "junk" value is meant to be large enough to force out-of-line storage
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid, junk) VALUES (1, 1, repeat(pi()::text,20000));"
);

$node_publisher->wait_for_catchup('tap_sub');

# Check data on subscriber
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk;");
is($result, qq(1|1|1), 'check replicated tab_fk inserts on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(1|1|1), 'check replicated tab_fk_ref inserts on subscriber');

# Drop the fk on publisher
$node_publisher->safe_psql('postgres', "DROP TABLE tab_fk CASCADE;");

# Insert data
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid) VALUES (2, 2);");

$node_publisher->wait_for_catchup('tap_sub');

# FK is not enforced on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2), 'check FK ignored on subscriber');

# Add replica trigger
$node_subscriber->safe_psql(
	'postgres', qq{
CREATE FUNCTION filter_basic_dml_fn() RETURNS TRIGGER AS \$\$
BEGIN
    IF (TG_OP = 'INSERT') THEN
        IF (NEW.id < 10) THEN
            RETURN NEW;
        ELSE
            RETURN NULL;
        END IF;
    ELSIF (TG_OP = 'UPDATE') THEN
        RETURN NULL;
    ELSE
        RAISE WARNING 'Unknown action';
        RETURN NULL;
    END IF;
END;
\$\$ LANGUAGE plpgsql;
CREATE TRIGGER filter_basic_dml_trg
    BEFORE INSERT OR UPDATE OF bid ON tab_fk_ref
    FOR EACH ROW EXECUTE PROCEDURE filter_basic_dml_fn();
ALTER TABLE tab_fk_ref ENABLE REPLICA TRIGGER filter_basic_dml_trg;
});

# Insert data
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid) VALUES (10, 10);");

$node_publisher->wait_for_catchup('tap_sub');

# The trigger should cause the insert to be skipped on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2), 'check replica insert trigger applied on subscriber');

# Update data
$node_publisher->safe_psql('postgres',
	"UPDATE tab_fk_ref SET bid = 2 WHERE bid = 1;");

$node_publisher->wait_for_catchup('tap_sub');

# The trigger should cause the update to be skipped on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2),
	'check replica update column trigger applied on subscriber');

# Update on a column not specified in the trigger, but it will trigger
# anyway because logical replication ships all columns in an update.
$node_publisher->safe_psql('postgres',
	"UPDATE tab_fk_ref SET id = 6 WHERE id = 1;");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(id), max(id) FROM tab_fk_ref;");
is($result, qq(2|1|2),
	'check column trigger applied even on update for other column');

# Create all three initially deferred constraints on both nodes.
for my $node ($node_publisher, $node_subscriber)
{
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE tab_deferred (id int, val int, span int4range,
    CONSTRAINT deferred_primary PRIMARY KEY (id) DEFERRABLE INITIALLY DEFERRED,
    CONSTRAINT deferred_unique UNIQUE (val) DEFERRABLE INITIALLY DEFERRED,
    CONSTRAINT deferred_exclusion EXCLUDE USING gist (span WITH &&) DEFERRABLE INITIALLY DEFERRED);
));
}

# Stop apply after a constraint violation so the failed insert can be checked.
$node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION;
	ALTER SUBSCRIPTION tap_sub SET (disable_on_error = true);
));
my $disabled_query =
  "SELECT NOT subenabled FROM pg_subscription WHERE subname = 'tap_sub'";
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Check the firing mode of each constraint's internal recheck trigger.
$result = $node_subscriber->safe_psql(
	'postgres',
	"SELECT tgenabled FROM pg_trigger
	 WHERE tgrelid = 'tab_deferred'::regclass
	 AND tgfoid = 'unique_key_recheck'::regproc");
is($result, qq(A\nA\nA),
	'all three recheck triggers are always enabled on subscriber');

# Insert the subscriber-only rows for all three conflicts at once.
$node_subscriber->safe_psql(
	'postgres',
	"INSERT INTO tab_deferred VALUES
	 (1, 99, '[90,91)'), (98, 20, '[91,92)'), (99, 98, '[3,4)')");

# The incoming row duplicates the primary key of a subscriber-only row.
my $log_offset = -s $node_subscriber->logfile;
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_deferred VALUES (1, 10, '[1,2)')");
$node_subscriber->wait_for_log(
	qr/duplicate key value violates unique constraint "deferred_primary"/,
	$log_offset);
$node_subscriber->poll_query_until('postgres', $disabled_query)
  or die 'Timed out waiting for subscription to be disabled';
$node_subscriber->safe_psql(
	'postgres', qq(
	DELETE FROM tab_deferred WHERE id = 1;
	ALTER SUBSCRIPTION tap_sub ENABLE;
));
$node_publisher->wait_for_catchup('tap_sub');

# The incoming row has a different primary key but duplicates the unique value.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_deferred VALUES (2, 20, '[2,3)')");
$node_subscriber->wait_for_log(
	qr/duplicate key value violates unique constraint "deferred_unique"/,
	$log_offset);
$node_subscriber->poll_query_until('postgres', $disabled_query)
  or die 'Timed out waiting for subscription to be disabled';
$node_subscriber->safe_psql(
	'postgres', qq(
	DELETE FROM tab_deferred WHERE id = 98;
	ALTER SUBSCRIPTION tap_sub ENABLE;
));
$node_publisher->wait_for_catchup('tap_sub');

# Only the range overlaps with the subscriber-only row.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_deferred VALUES (3, 30, '[3,4)')");
$node_subscriber->wait_for_log(
	qr/conflicting key value violates exclusion constraint "deferred_exclusion"/,
	$log_offset);
$node_subscriber->poll_query_until('postgres', $disabled_query)
  or die 'Timed out waiting for subscription to be disabled';
$node_subscriber->safe_psql(
	'postgres', qq(
	DELETE FROM tab_deferred WHERE id = 99;
	ALTER SUBSCRIPTION tap_sub ENABLE;
));
$node_publisher->wait_for_catchup('tap_sub');

# Adding an initially immediate constraint to a subscribed table must fail.
my ($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"ALTER TABLE tab_deferred ADD CONSTRAINT immediate_unique UNIQUE (val) DEFERRABLE INITIALLY IMMEDIATE"
);
like(
	$stderr,
	qr/cannot create constraint "immediate_unique" on logical replication target relation "tab_deferred"/,
	'constraint DDL reports subscription target restriction');

# Cleanup
$node_subscriber->safe_psql('postgres',
	'DROP SUBSCRIPTION tap_sub; DROP TABLE tab_deferred');
$node_publisher->safe_psql('postgres',
	'DROP TABLE tab_deferred; DROP PUBLICATION tap_pub');

# Create one initially immediate constraint per table on both nodes.
for my $node ($node_publisher, $node_subscriber)
{
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE immediate_primary (val int PRIMARY KEY DEFERRABLE INITIALLY IMMEDIATE);
CREATE TABLE immediate_unique (val int UNIQUE DEFERRABLE INITIALLY IMMEDIATE);
CREATE TABLE immediate_exclusion (val int4range, EXCLUDE USING gist (val WITH &&) DEFERRABLE INITIALLY IMMEDIATE);
));
}

# Publish only one table at a time, and reject each subscription attempt.
$node_publisher->safe_psql('postgres',
	'CREATE PUBLICATION tap_pub FOR TABLE immediate_primary');
($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION rejected_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);
like(
	$stderr,
	qr/Deferrable initially immediate unique, primary key, and exclusion constraints are not supported/,
	'primary key reports unsupported timing during subscription creation');

$node_publisher->safe_psql('postgres',
	'ALTER PUBLICATION tap_pub SET TABLE immediate_unique');
($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION rejected_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);
like(
	$stderr,
	qr/Deferrable initially immediate unique, primary key, and exclusion constraints are not supported/,
	'unique constraint reports unsupported timing during subscription creation'
);

$node_publisher->safe_psql('postgres',
	'ALTER PUBLICATION tap_pub SET TABLE immediate_exclusion');
($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION rejected_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);
like(
	$stderr,
	qr/Deferrable initially immediate unique, primary key, and exclusion constraints are not supported/,
	'exclusion constraint reports unsupported timing during subscription creation'
);

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
