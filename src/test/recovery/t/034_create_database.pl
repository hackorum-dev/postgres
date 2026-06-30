
# Copyright (c) 2023-2026, PostgreSQL Global Development Group

# Test WAL replay for CREATE DATABASE .. STRATEGY WAL_LOG.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

# This checks that any DDLs run on the template database that modify pg_class
# are persisted after creating a database from it using the WAL_LOG strategy,
# as a direct copy of the template database's pg_class is used in this case.
my $db_template = "template1";
my $db_new = "test_db_1";

# Create table.  It should persist on the template database.
$node->safe_psql("postgres",
	"CREATE DATABASE $db_new STRATEGY WAL_LOG TEMPLATE $db_template;");

$node->safe_psql($db_template, "CREATE TABLE tab_db_after_create_1 (a INT);");

# Flush the changes affecting the template database, then replay them.
$node->safe_psql("postgres", "CHECKPOINT;");

$node->stop('immediate');
$node->start;
my $result = $node->safe_psql($db_template,
	"SELECT count(*) FROM pg_class WHERE relname LIKE 'tab_db_%';");
is($result, "1",
	"check that table exists on template after crash, with checkpoint");

# The new database should have no tables.
$result = $node->safe_psql($db_new,
	"SELECT count(*) FROM pg_class WHERE relname LIKE 'tab_db_%';");
is($result, "0",
	"check that there are no tables from template on new database after crash"
);


# Verify that sequences in databases created with CREATE DATABASE ... TEMPLATE
# properly generate WAL records on first nextval(), ensuring that failover does
# not cause sequence value rollback.

# Set up streaming replication
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;

# Create and advance a sequence on template database
$node_primary->safe_psql('postgres', 'CREATE DATABASE template_db');
$node_primary->safe_psql('template_db', 'CREATE SEQUENCE test_seq');
for (my $i = 0; $i < 5; $i++) {
	$node_primary->safe_psql('template_db',
		"SELECT nextval('test_seq')");
}

my $tmpl_last_value = $node_primary->safe_psql('template_db',
	'SELECT last_value FROM test_seq');
is($tmpl_last_value, '5', 'the sequence last_value is 5');

my $tmpl_log_cnt = $node_primary->safe_psql('template_db',
	'SELECT log_cnt FROM test_seq');
ok($tmpl_log_cnt > 0, "the sequence has pre-logged values (log_cnt=$tmpl_log_cnt)");

# Create new database test_db by CREATE DATABASE ... TEMPLATE and verify initial state
$node_primary->safe_psql('postgres',
	'CREATE DATABASE test_db TEMPLATE template_db');

my $new_last_value_before = $node_primary->safe_psql('test_db',
	'SELECT last_value FROM test_seq');
is($new_last_value_before, '5',
	'test_db sequence last_value starts at 5 (same as template)');

# Call nextval() on primary and verify if standby's last_value >= primary's value
for (my $i = 0; $i < 5; $i++) {
	$node_primary->safe_psql('test_db', "SELECT nextval('test_seq')");
}

my $primary_last_value = $node_primary->safe_psql('test_db',
	'SELECT last_value FROM test_seq');
is($primary_last_value, '10',
	'primary test_db sequence advanced to 10 after 5 nextval calls');

$node_primary->wait_for_catchup($node_standby);

my $standby_last_value = $node_standby->safe_psql('test_db',
	'SELECT last_value FROM test_seq');
ok($standby_last_value >= $primary_last_value,
	"standby sequence last_value ($standby_last_value) >= primary ($primary_last_value) - WAL was generated");
ok($standby_last_value > 5,
	"standby sequence was updated after nextval on primary (value=$standby_last_value)");

# Failover and verify if nextval() should return a value larger than previous primary's value
$node_primary->stop;
$node_standby->promote;

my $promoted_nextval = $node_standby->safe_psql('test_db',
	"SELECT nextval('test_seq')");
ok($promoted_nextval > 10,
	"after failover, nextval returns $promoted_nextval (> 10, no rollback)");

$node_standby->safe_psql('postgres', 'DROP DATABASE test_db');
$node_standby->safe_psql('postgres', 'DROP DATABASE template_db');

done_testing();
