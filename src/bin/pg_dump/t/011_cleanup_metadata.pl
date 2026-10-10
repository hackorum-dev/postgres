# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q{log_statement = 'all'
log_line_prefix = '%a '
});
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE TABLE cleanup_probe_a (id integer);
CREATE TABLE cleanup_probe_b (id integer);
INSERT INTO cleanup_probe_a VALUES (77);
INSERT INTO cleanup_probe_b VALUES (77);
});

# A directory archive with SQL entries exercises the reader and planner
# without data files or custom-format offsets.  Its toc.dat uses the tar format
# code.  Versions 1.16 and 1.17 differ only in their cleanup fields here.
sub archive_int
{
	my ($value) = @_;
	return pack('CV', $value < 0, abs($value));
}

sub archive_str
{
	my ($value) = @_;
	return archive_int(-1) unless defined $value;
	return archive_int(length($value)) . $value;
}

sub write_archive
{
	my ($name, $minor, @entries) = @_;
	# Real dumps contain encoding and string settings, processed even with
	# --use-list.  These initialize SQL quoting as well as server settings.
	unshift @entries,
	  {
		id => 100,
		tag => 'STDSTRINGS',
		desc => 'STDSTRINGS',
		defn => "SET standard_conforming_strings = 'on';\n",
		drop => ''
	  };
	unshift @entries,
	  {
		id => 101,
		tag => 'ENCODING',
		desc => 'ENCODING',
		defn => "SET client_encoding = 'UTF8';\n",
		drop => ''
	  };
	my $dir = "$tempdir/$name";
	mkdir $dir or die "mkdir $dir: $!";
	my $toc = pack('a5C7', 'PGDMP', 1, $minor, 0, 4, 8, 3, 0);
	$toc .= archive_int($_) for (0, 0, 0, 1, 0, 126, 0);
	$toc .= archive_str($_) for ('postgres', '20devel', '20devel');
	$toc .= archive_int(scalar @entries);

	for my $entry (@entries)
	{
		my $tag = $entry->{tag} // 'cleanup_probe_a';
		$toc .= archive_int($entry->{id}) . archive_int(0);
		$toc .= archive_str($_)
		  for ('0', '0', $tag, $entry->{desc} // 'TABLE');
		$toc .= archive_int($entry->{desc} ? 1 : 2);    # NONE or PRE_DATA
		$toc .=
		  archive_str($entry->{defn}
			  // "CREATE TABLE public.$tag (id integer); INSERT INTO public.$tag VALUES (77);\n"
		  );
		$toc .= archive_str($entry->{drop} // "DROP TABLE public.$tag;\n");
		$toc .= archive_str($_) for (undef, 'public', '', undef);
		$toc .= archive_int(ord('r'));
		$toc .= archive_str($_) for ('', 'false', undef);

		if ($minor >= 17)
		{
			my @alternatives = @{ $entry->{alternatives} // [] };
			my @deps = @{ $entry->{deps} // [] };
			$toc .= archive_int($entry->{kind}  // 0);
			$toc .= archive_int($entry->{owner} // 0);
			$toc .=
			  archive_int($entry->{nalternatives} // scalar @alternatives);
			$toc .= archive_int($_) for @alternatives;
			$toc .= archive_int($entry->{ndeps} // scalar @deps);
			$toc .= archive_int($_) for @deps;
		}
		$toc .= archive_str('');    # no data filename
	}
	open my $fh, '>:raw', "$dir/toc.dat" or die "open $dir/toc.dat: $!";
	print $fh $toc;
	close $fh or die "close $dir/toc.dat: $!";
	return $dir;
}

my $snapshot_query = q{
SELECT oid FROM pg_class WHERE relname LIKE 'cleanup_probe_%' ORDER BY oid;
SELECT * FROM cleanup_probe_a;
SELECT * FROM cleanup_probe_b;
};
my $snapshot = $node->safe_psql('postgres', $snapshot_query);
my $selection = "$tempdir/only-a.list";
append_to_file($selection, "1; 0 0 TABLE public cleanup_probe_a\n");

# Confirm that the SQL log assertion below can observe a known statement.
my $control_offset = -s $node->logfile;
$node->command_ok(
	[
		'psql', '-X',
		'--dbname' => $node->connstr('postgres')
		  . ' application_name=cleanup_metadata_failure',
		'-c', 'SELECT 1'
	],
	'execute SQL logging control');
like(
	slurp_file($node->logfile, $control_offset),
	qr/cleanup_metadata_failure .*statement: SELECT 1/,
	'SQL logging observes commands from the test application');

sub rejected_archive
{
	my ($name, $error, $entries, @options) = @_;
	my $archive = write_archive($name, 17, @$entries);
	my $log_offset = -s $node->logfile;
	$node->command_fails_like(
		[
			'pg_restore', '--clean', '--if-exists', @options,
			'--dbname' => $node->connstr('postgres')
			  . ' application_name=cleanup_metadata_failure',
			$archive
		],
		$error, $name);
	unlike(
		slurp_file($node->logfile, $log_offset),
		qr/cleanup_metadata_failure .*statement:/,
		"$name emits no SQL to the server");
	is($node->safe_psql('postgres', $snapshot_query),
		$snapshot, "$name leaves object OIDs and rows unchanged");
}

# Reader validation, including the distinction between forbidden negative IDs
# and the zero prerequisite sentinel, which the planner must diagnose instead.
for my $invalid (
	[ 'invalid-kind', { kind => 99 } ],
	[ 'negative-kind', { kind => -1 } ],
	[ 'negative-owner', { owner => -1 } ],
	[ 'negative-alternative-count', { nalternatives => -1 } ],
	[ 'negative-dependency-count', { ndeps => -1 } ])
{
	rejected_archive(
		$invalid->[0],
		qr/invalid cleanup metadata for TOC entry 1/,
		[ { id => 1, %{ $invalid->[1] } } ]);
}
for my $invalid ([ 'zero-alternative', 0 ], [ 'negative-alternative', -1 ])
{
	rejected_archive(
		$invalid->[0],
		qr/invalid cleanup alternative for TOC entry 1/,
		[ { id => 1, alternatives => [ $invalid->[1] ] } ]);
}
rejected_archive(
	'negative-dependency',
	qr/invalid cleanup dependency for TOC entry 1/,
	[ { id => 1, deps => [-1] } ]);

my $probe_b = { id => 2, tag => 'cleanup_probe_b' };
rejected_archive(
	'unresolved-dependency',
	qr/Required cleanup metadata could not be resolved \(DumpId 0\)/,
	[ { id => 1, kind => 2, deps => [0] }, $probe_b ]);
rejected_archive(
	'missing-dependency',
	qr/Cleanup entry with DumpId 99 is not present in the archive/,
	[ { id => 1, kind => 2, deps => [99] }, $probe_b ]);
rejected_archive(
	'unselected-dependency',
	qr/Cleanup entry "cleanup_probe_b" \(DumpId 2\) is not selected for cleanup/,
	[ { id => 1, kind => 2, deps => [2] }, $probe_b ],
	'--use-list' => $selection);
rejected_archive(
	'covered-dependency-without-own-drop',
	qr/Cleanup entry "cleanup_probe_b" \(DumpId 2\) has no independent DROP action/,
	[
		{ id => 1, kind => 2, deps => [2] },
		{ id => 2, tag => 'cleanup_probe_b', kind => 1, owner => 3 },
		{ id => 3, tag => 'cleanup_probe_cover' }
	]);
rejected_archive(
	'self-cycle',
	qr/cycle in cleanup dependencies for "cleanup_probe_a"/,
	[ { id => 1, kind => 2, deps => [1] } ]);
rejected_archive(
	'two-node-cycle',
	qr/cycle in cleanup dependencies/,
	[
		{ id => 1, kind => 2, deps => [2] },
		{ id => 2, tag => 'cleanup_probe_b', kind => 2, deps => [1] }
	]);
rejected_archive(
	'missing-covering-entry',
	qr/Cleanup entry with DumpId 99 is not present in the archive/,
	[ { id => 1, kind => 1, owner => 99 } ]);
rejected_archive(
	'unselected-covering-entry',
	qr/Cleanup entry "cleanup_probe_b" \(DumpId 2\) is not selected for cleanup/,
	[ { id => 1, kind => 1, owner => 2 }, $probe_b ],
	'--use-list' => $selection);

# An empty target still cannot waive source-metadata requirements.  A nonexistent
# database additionally proves that planning happens before connecting at all.
my $incomplete =
  write_archive('incomplete', 17, { id => 1, kind => 2, deps => [99] });
$node->safe_psql('postgres', 'CREATE DATABASE empty_target');
for my $dbname ('empty_target', 'database_that_does_not_exist')
{
	$node->command_fails_like(
		[
			'pg_restore', '--clean', '--if-exists',
			'--dbname' => $node->connstr($dbname),
			$incomplete
		],
		qr/Cleanup entry with DumpId 99 is not present in the archive/,
		"source cleanup check precedes connecting to $dbname");
}

# Check both archive layouts by actual repeated restoration, not just --list.
# The legacy writer omits all cleanup fields; the reader must supply DIRECT
# defaults even though this fixture's unsaved metadata would require coverage.
for my $minor (16, 17)
{
	my $dbname = "compat_$minor";
	$node->safe_psql('postgres', "CREATE DATABASE $dbname");
	my $legacy = $minor == 16 ? { kind => 1, owner => 99 } : {};
	my $archive =
	  write_archive("compat-$minor", $minor, { id => 1, %$legacy }, $probe_b);
	$node->command_ok(
		[
			'pg_restore',
			'--dbname' => $node->connstr($dbname),
			'--exit-on-error', $archive
		],
		"initial 1.$minor restore");
	my $before = $node->safe_psql($dbname, $snapshot_query);
	$node->command_ok(
		[
			'pg_restore', '--clean',
			'--dbname' => $node->connstr($dbname),
			'--exit-on-error', $archive
		],
		"repeat 1.$minor clean restore");
	isnt($node->safe_psql($dbname, $snapshot_query),
		$before, "1.$minor actually recreates tables");
	is( $node->safe_psql(
			$dbname,
			'SELECT * FROM cleanup_probe_a UNION ALL SELECT * FROM cleanup_probe_b'
		),
		"77\n77",
		"1.$minor restores fixture data");
}

done_testing();
