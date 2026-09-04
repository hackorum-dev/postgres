# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Set up node P as primary
my $node_p = PostgreSQL::Test::Cluster->new('node_p');
my $pconnstr = $node_p->connstr;
$node_p->init(allows_streaming => 'logical');

# Remove pgoutput from output_plugin_libraries
$node_p->append_conf('postgresql.conf', "output_plugin_libraries = 'test_decoding'");
$node_p->start;

# Set up node S as standby linking to node P
$node_p->backup('backup');
my $node_s = PostgreSQL::Test::Cluster->new('node_s');
$node_s->init_from_backup($node_p, 'backup', has_streaming => 1);

# --dry-run succeeds
command_ok(
	[
		'pg_createsubscriber',
		'--verbose',
        '--dry-run',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $pconnstr,
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--database' => 'postgres',
	],
	'run pg_createsubscriber --dry-run on node S');


# ... but actual run fails
command_ok(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $pconnstr,
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--database' => 'postgres',
	],
	'run pg_createsubscriber on node S');

done_testing();
