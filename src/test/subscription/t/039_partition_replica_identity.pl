# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# Test that, under publish_via_partition_root, pgoutput chooses the old-tuple
# flag ('O' full old tuple vs 'K' replica-identity key) from the *leaf*
# partition's replica identity rather than the root's.
#
# The change is published under the root's OID and tuple descriptor, but the
# old-tuple payload is built from the leaf that actually stored the row, so the
# flag has to match that leaf.  A leaf with REPLICA IDENTITY FULL must emit 'O';
# a leaf using its replica-identity key must emit 'K'.  This is a wire-protocol
# conformance property: the built-in apply worker only checks the flag to learn
# whether an old tuple is present, so both leaves still replicate correctly.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $pub = PostgreSQL::Test::Cluster->new('publisher');
$pub->init(allows_streaming => 'logical');
$pub->start;

my $sub = PostgreSQL::Test::Cluster->new('subscriber');
$sub->init;
$sub->start;

# We pick a root identity that both leaves cover, as required under
# publish_via_partition_root: the root uses its default identity, the primary
# key on (id, ts).  part_table_sect_1 stores 'first' and has the stronger
# REPLICA IDENTITY FULL, so its old tuples must be tagged 'O'.
# part_table_sect_2 stores 'second' and keeps the default identity (the same
# primary key), so its old tuples must be tagged 'K'.
$pub->safe_psql(
	'postgres', q{
create table part_table(
  id  int generated always as identity,
  ts  timestamp,
  load text,
  constraint part_table_pk primary key(id, ts)
) partition by range(ts);

create table part_table_sect_1 partition of part_table
  for values from ('2000-01-01') to ('2024-01-01');
create table part_table_sect_2 partition of part_table
  for values from ('2024-01-01') to (maxvalue);

alter table part_table_sect_1 replica identity full;

create publication pub_part_table
  for table part_table
  with (publish_via_partition_root = true);
});

# A dedicated slot lets us read the raw pgoutput stream and inspect the
# old-tuple flag of each UPDATE/DELETE message.
$pub->safe_psql('postgres',
	q{select pg_create_logical_replication_slot('slot_test', 'pgoutput');});

$sub->safe_psql(
	'postgres', q{
create table part_table(
  id  int,
  ts  timestamp,
  load text,
  constraint part_table_pk primary key(id, ts)
) partition by range(ts);

create table part_table_sect_1 partition of part_table
  for values from ('2000-01-01') to ('2024-01-01');
create table part_table_sect_2 partition of part_table
  for values from ('2024-01-01') to (maxvalue);
});

my $connstr = $pub->connstr . ' dbname=postgres';
$sub->safe_psql(
	'postgres', qq{
create subscription sub_part
  connection '$connstr application_name=sub_part'
  publication pub_part_table;});

$sub->wait_for_subscription_sync($pub, 'sub_part');

# Return the ordered publisher-relevant contents of both nodes' tables, so we
# can assert the subscriber stays in step with the publisher.
sub dump_table
{
	my ($node) = @_;
	return $node->safe_psql('postgres',
		q{select id, to_char(ts, 'YYYY-MM-DD HH24:MI') as ts, load
		    from part_table order by id, ts});
}

# Consume the messages decoded since the previous call and return the list of
# old-tuple flags of the UPDATE/DELETE messages found, e.g. ('UO', 'DK').  With
# proto_version 1 and no streaming, an UPDATE/DELETE message is a one-byte type
# followed by the 4-byte relation OID and then the old-tuple flag byte.
sub old_tuple_flags
{
	my ($node) = @_;
	my $hexdump = $node->safe_psql(
		'postgres', q{
		select encode(data, 'hex')
		  from pg_logical_slot_get_binary_changes('slot_test', null, null,
		         'proto_version', '1',
		         'publication_names', 'pub_part_table')});
	my @flags;
	foreach my $hex (split /\n/, $hexdump)
	{
		next if $hex eq '';
		my $data = pack('H*', $hex);
		my $type = substr($data, 0, 1);
		next unless $type eq 'U' or $type eq 'D';
		push @flags, $type . substr($data, 5, 1);
	}
	return \@flags;
}

$pub->safe_psql(
	'postgres', q{
insert into part_table values (default, '2020-01-01 00:00', 'first');
insert into part_table values (default, '2025-01-01 00:00', 'second');
});
$pub->wait_for_catchup('sub_part');
is(dump_table($sub), dump_table($pub), 'subscriber matches publisher after insert');

# Drop the INSERT messages we are not interested in.
old_tuple_flags($pub);

# UPDATE touching the replica-identity key column (ts) of each leaf, one leaf
# per transaction so we can attribute each message to a known partition.
$pub->safe_psql('postgres',
	q{update part_table set ts = ts + interval '1 hour' where load = 'first';});
is_deeply(old_tuple_flags($pub), ['UO'],
	'UPDATE on leaf with REPLICA IDENTITY FULL uses O (full old tuple)');

$pub->safe_psql('postgres',
	q{update part_table set ts = ts + interval '1 hour' where load = 'second';});
is_deeply(old_tuple_flags($pub), ['UK'],
	'UPDATE on leaf with key replica identity uses K (old key only)');

$pub->wait_for_catchup('sub_part');
is(dump_table($sub), dump_table($pub), 'subscriber matches publisher after update');

# DELETE always carries an old tuple; again one leaf per transaction.
$pub->safe_psql('postgres',
	q{delete from part_table where load = 'first';});
is_deeply(old_tuple_flags($pub), ['DO'],
	'DELETE on leaf with REPLICA IDENTITY FULL uses O (full old tuple)');

$pub->safe_psql('postgres',
	q{delete from part_table where load = 'second';});
is_deeply(old_tuple_flags($pub), ['DK'],
	'DELETE on leaf with key replica identity uses K (old key only)');

$pub->wait_for_catchup('sub_part');
is($sub->safe_psql('postgres', 'select count(*) from part_table'),
	'0', 'subscriber has no rows left after delete');
is(dump_table($sub), dump_table($pub), 'subscriber matches publisher after delete');

done_testing();
