#
# Test that DDL arranges for sufficient syncs.
#
use strict;
use warnings;
use PostgresNode;
use Test::More;
use TestLib;

plan tests => 6;

my $node = PostgresNode->get_new_node('solo');
$node->init();
# Avoid time-based checkpoints, so checkpoints happen when we choose.
$node->append_conf('postgresql.conf', 'checkpoint_timeout = 60min');
# If fsync were disabled, the server would skip relevant code.
$node->append_conf('postgresql.conf', 'fsync = on');
# Log the "checkpoint sync:" messages.
$node->append_conf('postgresql.conf', 'log_checkpoints = on');
$node->append_conf('postgresql.conf', 'log_min_messages = debug1');
# Test at the default wal_level.
$node->append_conf('postgresql.conf', 'wal_level = replica');

# UNLOGGED relations.  The variants with an INSERT after the DDL are the easy
# case, because the INSERT arranges for a sync.  The others rely on DDL making
# correct arrangements.
$node->start;
my $tablespace_dir = $node->basedir . '/tablespace_other';
mkdir($tablespace_dir);
$tablespace_dir = TestLib::perl2host($tablespace_dir);
$node->safe_psql('postgres',
				 "CREATE TABLESPACE other LOCATION '$tablespace_dir';");
my $tablespace_insert_filepath = $node->safe_psql('postgres', <<EOSQL);
CREATE UNLOGGED TABLE spc_ins (c int);
INSERT INTO spc_ins VALUES (1);
ALTER TABLE spc_ins SET TABLESPACE other;
INSERT INTO spc_ins VALUES (2);
SELECT pg_relation_filepath('spc_ins');
EOSQL
my $tablespace_filepath = $node->safe_psql('postgres', <<EOSQL);
CREATE UNLOGGED TABLE spc (c int);
INSERT INTO spc VALUES (1);
ALTER TABLE spc SET TABLESPACE other;
SELECT pg_relation_filepath('spc');
EOSQL
my $cluster_insert_filepath = $node->safe_psql('postgres', <<EOSQL);
CREATE UNLOGGED TABLE clu_ins (c int PRIMARY KEY);
INSERT INTO clu_ins VALUES (1);
CLUSTER clu_ins USING clu_ins_pkey;
INSERT INTO clu_ins VALUES (2);
SELECT pg_relation_filepath('clu_ins');
EOSQL
my $cluster_filepath = $node->safe_psql('postgres', <<EOSQL);
CREATE UNLOGGED TABLE clu (c int PRIMARY KEY);
INSERT INTO clu VALUES (1);
CLUSTER clu USING clu_pkey;
SELECT pg_relation_filepath('clu');
EOSQL
my $index_insert_filepath = $node->safe_psql('postgres', <<EOSQL);
SELECT pg_relation_filepath('clu_ins_pkey');
EOSQL
my $index_filepath = $node->safe_psql('postgres', <<EOSQL);
SELECT pg_relation_filepath('clu_pkey');
EOSQL
$node->stop;
like(slurp_file($node->logfile),
	 qr/checkpoint sync: number=[0-9]* file=$tablespace_insert_filepath/,
	 'sync unlogged table after SET TABLEPSACE+INSERT');
like(slurp_file($node->logfile),
	 qr/checkpoint sync: number=[0-9]* file=$tablespace_filepath/,
	 'sync unlogged table after SET TABLESPACE');
like(slurp_file($node->logfile),
	 qr/checkpoint sync: number=[0-9]* file=$cluster_insert_filepath/,
	 'sync unlogged table after CLUSTER+INSERT');
like(slurp_file($node->logfile),
	 qr/checkpoint sync: number=[0-9]* file=$cluster_filepath/,
	 'sync unlogged table after CLUSTER');
like(slurp_file($node->logfile),
	 qr/checkpoint sync: number=[0-9]* file=$index_insert_filepath/,
	 'sync unlogged index after CREATE+INSERT');
like(slurp_file($node->logfile),
	 qr/checkpoint sync: number=[0-9]* file=$index_filepath/,
	 'sync unlogged index after CREATE');
