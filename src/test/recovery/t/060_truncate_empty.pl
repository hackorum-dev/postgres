use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(gettimeofday tv_interval);

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init();

$node->append_conf('postgresql.conf', 'shared_buffers = 4GB');
$node->append_conf('postgresql.conf', 'restart_after_crash = on');

$node->start();

$node->safe_psql('postgres',
    q[CREATE TABLE test (id int);]);

# SIGSTOP checkpointer and run some transactions
my $checkpointer_pid = $node->safe_psql('postgres',
    q[SELECT pid FROM pg_stat_activity WHERE backend_type = 'checkpointer';]);
chomp($checkpointer_pid);
kill 'STOP', $checkpointer_pid;
note("Checkpointer stopped");

$node->pgbench(
    '--no-vacuum --client=10 --transactions=1000',
    0,
    [qr{actually processed}],
    [qr{^$}],
    'concurrent CREATE and DROP TABLE transactions',
    {
        'truncate_empty_script' => q(
            BEGIN;
            INSERT INTO test VALUES (:client_id);
            DELETE FROM test WHERE id = :client_id;
            CREATE TABLE test_empty_:client_id (id int);
            DROP TABLE test_empty_:client_id;
            COMMIT;
        )
    });

# stop the node in immediate mode for crash recovery
$node->stop('immediate');

my $recovery_start = [gettimeofday];
$node->start();
my $recovery_end = [gettimeofday];
my $recovery_time = tv_interval($recovery_start, $recovery_end);

note("Crash recovery time: ${recovery_time} seconds");

my $log_content = $node->log_content();
if ($log_content =~ /redo done at .+? system usage: CPU: user: ([\d.]+) s, system: ([\d.]+) s, elapsed: ([\d.]+) s/m)
{
    my $cpu_user = $1;
    my $cpu_system = $2;
    my $redo_elapsed = $3;
    
    note("Redo elapsed time: $redo_elapsed s");
    note("  CPU user: $cpu_user s, system: $cpu_system s");
}

# consistency check
my $result = $node->safe_psql('postgres', q[SELECT COUNT(*) FROM test;]);
is($result, '0', 'test table is empty after recovery');

$node->stop();
done_testing();
