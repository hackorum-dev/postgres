# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that an error relayed from a parallel apply worker (applying a
# streamed transaction) to the leader does not get a misleading extra
# CONTEXT line describing some other, unrelated transaction the leader
# happens to be applying directly at the same time. This currently fails.
#

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	'logical_decoding_work_mem = 64kB');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;
$node_subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points');

foreach my $node ($node_publisher, $node_subscriber)
{
	$node->safe_psql('postgres', qq(
		CREATE TABLE tab_a (a int PRIMARY KEY, data bytea);
		CREATE TABLE tab_b (a int PRIMARY KEY);
	));
}

# tab_a: the transaction that actually fails. Streamed to the parallel
# apply worker (the bulk insert below exceeds logical_decoding_work_mem).
$node_subscriber->safe_psql('postgres', qq[
	CREATE OR REPLACE FUNCTION tab_a_boom_fn()
	RETURNS trigger
	LANGUAGE plpgsql
	AS \$\$
	BEGIN
		RAISE EXCEPTION 'tab_a trigger boom';
	END;
	\$\$;

	CREATE TRIGGER tab_a_boom_tg
	BEFORE INSERT ON tab_a
	FOR EACH ROW
	EXECUTE FUNCTION tab_a_boom_fn();
]);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_a ENABLE REPLICA TRIGGER tab_a_boom_tg;");

$node_publisher->safe_psql('postgres',
	'CREATE PUBLICATION pub FOR TABLE tab_a, tab_b');

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$publisher_connstr' PUBLICATION pub WITH (streaming = on)"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub');

# pa_can_start() (applyparallelworker.c) will only ever launch a parallel
# worker if AllTablesyncsReady() sees the tables as READY, and that check
# reads a cache (relation_states_validity, syncutils.c) that's only
# refreshed via catalog invalidation delivery -- it isn't guaranteed fresh
# just because wait_for_subscription_sync() above returned.
my $oldpid = $node_publisher->safe_psql('postgres',
	"SELECT pid FROM pg_stat_replication WHERE application_name = 'sub' AND state = 'streaming'"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION sub SET (streaming = parallel)");
$node_publisher->poll_query_until('postgres',
	"SELECT pid != $oldpid FROM pg_stat_replication WHERE application_name = 'sub' AND state = 'streaming'"
) or die "Timed out while waiting for apply to restart after changing SUBSCRIPTION";

# Freeze the parallel apply worker right after it is assigned the tab_a
# transaction's first stream segment, before it drains anything.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-stream-start', 'wait')"
);

# Freeze the leader itself right after it sets remote_ctx to describe
# tab_b, while actually applying tab_b's insert directly.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('leader-apply-insert-remote-ctx-set', 'wait')"
);

my $log_offset = -s $node_subscriber->logfile;

# TX1: a bulk insert into tab_a, large enough to exceed
# logical_decoding_work_mem and so get streamed to the parallel worker,
# which immediately freezes before touching anything. Kept open (not
# committed yet) via a background session: in base PostgreSQL, the leader
# unconditionally waits for a streamed transaction's worker to finish once
# that transaction's own STREAM COMMIT message is processed
# (pa_xact_finish() -> pa_wait_for_xact_finish(), see applyparallelworker.c)
# -- so TX2 below needs to commit, and be seen as frozen by the leader,
# while TX1 is still open, or the leader would never reach it at all.
my $h = $node_publisher->background_psql('postgres', on_error_stop => 0);
$h->query_safe(
	q{
BEGIN;
INSERT INTO tab_a SELECT i, sha256(i::text::bytea) FROM generate_series(1, 1000) i;
});
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-stream-start');

# TX2: an unrelated, perfectly fine, non-streamed transaction, committed
# (on a separate connection) while TX1 above is still open. Always applied
# directly by the leader. Freezes right after remote_ctx is set to
# describe tab_b.
$node_publisher->safe_psql('postgres', 'INSERT INTO tab_b VALUES (1)');
$node_subscriber->wait_for_event('logical replication apply worker',
	'leader-apply-insert-remote-ctx-set');

# While the leader is frozen there -- i.e. remote_ctx still describes
# tab_b/TX2 -- let the frozen worker proceed. It will try to apply the
# first row of the streamed tab_a transaction, hit the trigger, raise an
# error, and relay it to the leader asynchronously.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_wakeup('parallel-worker-before-stream-start')");

$node_subscriber->wait_for_log(
	qr/logical replication parallel apply worker exited due to error/,
	$log_offset);

my $log_contents = slurp_file($node_subscriber->logfile, $log_offset);

# The error did originate from the tab_a/TX1 worker ...
like(
	$log_contents,
	qr/tab_a trigger boom/,
	'the relayed error is the tab_a trigger exception'
);

# ... and it must not be polluted with an unrelated CONTEXT line describing
# whatever the leader itself happened to be doing at that moment (tab_b):
# the leader's own context-stack walk in ProcessParallelApplyMessage()
# currently appends exactly such a bogus line, so this fails today.
unlike(
	$log_contents,
	qr/relation "public\.tab_b"/,
	'the relayed error must not be annotated with the leader\'s own, '
	  . 'unrelated tab_b activity as additional context'
);

# TX1 never got to commit (its worker errored out instead); errors make
# the next step fail, so ignore them here.
$h->quit;

$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_detach('parallel-worker-before-stream-start')");
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_detach('leader-apply-insert-remote-ctx-set')");

$node_subscriber->stop('immediate');
$node_publisher->stop('immediate');

done_testing();
