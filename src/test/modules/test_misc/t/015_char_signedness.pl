# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that hashchar() and hashcharextended() interpret a high-bit "char" value
# according to the cluster's recorded default char signedness (see
# GetDefaultCharSignedness()), rather than the signedness the server happens to
# be compiled with.
#
# src/test/regress/sql/hash_func.sql has a companion check, but it can only ever
# exercise the "signed" branch: a freshly-initialized cluster, like the one used
# for the regular regression tests, always records "signed" (see
# WriteControlFile()).  Here we use pg_resetwal to force the recorded signedness
# to "unsigned" and confirm that the SQL-level result flips accordingly,
# independent of the actual platform running the test.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;

# Newly-initialized clusters unconditionally record "signed".
$node->start;
is($node->safe_psql('postgres',
		q{SELECT hashchar('\200'::"char") = hashint4(-128)}),
	't',
	'hashchar() on a high-bit "char" uses signed semantics when the'
	  . ' cluster default char signedness is signed');
is($node->safe_psql('postgres',
		q{SELECT hashcharextended('\200'::"char", 0) = hashint4extended(-128, 0)}
	),
	't',
	'hashcharextended() on a high-bit "char" uses signed semantics when the'
	  . ' cluster default char signedness is signed');
$node->stop;

# Force the recorded signedness to "unsigned" and check again.
command_ok(
	[
		'pg_resetwal',
		'--char-signedness' => 'unsigned',
		'--force',
		$node->data_dir
	],
	"set cluster's default char signedness to unsigned");

$node->start;
is($node->safe_psql('postgres',
		q{SELECT hashchar('\200'::"char") = hashint4(128)}),
	't',
	'hashchar() on a high-bit "char" uses unsigned semantics when the'
	  . ' cluster default char signedness is unsigned');
is($node->safe_psql('postgres',
		q{SELECT hashcharextended('\200'::"char", 0) = hashint4extended(128, 0)}
	),
	't',
	'hashcharextended() on a high-bit "char" uses unsigned semantics when'
	  . ' the cluster default char signedness is unsigned');
$node->stop;

done_testing();
