# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that messages are passed to backends by
# pg_terminate_backend, pg_cancel_backend

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init();
$node->start;

my ($stdout, $stderr);

# pg_terminate_backend with message
$node->psql('postgres',
	q[SELECT pg_terminate_backend(pg_backend_pid(), 0, 'Have you seen my coffee cup?');],
	stdout => \$stdout, stderr => \$stderr);
like($stderr, qr/Have you seen my coffee cup\?/,
	"terminate message appears in DETAIL of FATAL");

# pg_terminate_backend with empty message uses generic text
$stdout = '';
$stderr = '';
$node->psql('postgres',
	q[SELECT pg_terminate_backend(pg_backend_pid(), 0, '');],
	stdout => \$stdout, stderr => \$stderr);
like($stderr, qr/terminating connection due to administrator command/,
	"terminate without message uses generic text");
unlike($stderr, qr/DETAIL/,
	"no DETAIL line when message is empty");

# pg_cancel_backend with message
$stdout = '';
$stderr = '';
$node->psql('postgres',
	q[SELECT pg_cancel_backend(pg_backend_pid(), 'You have to wear some ridiculous tie');],
	stdout => \$stdout, stderr => \$stderr);
like($stderr, qr/You have to wear some ridiculous tie/,
	"cancel message appears in DETAIL of ERROR");

# Long message is truncated with a NOTICE
$stdout = '';
$stderr = '';
my $longstr = "a" x 200;
my $truncated = "a" x 127;
$node->psql('postgres',
	qq[SELECT pg_terminate_backend(pg_backend_pid(), 0, '$longstr');],
	stdout => \$stdout, stderr => \$stderr);
like($stderr, qr/NOTICE:  message is too long, truncated to 127 bytes/,
	"NOTICE emitted for truncated message");
like($stderr, qr/\Q$truncated\E/,
	"truncated message (127 bytes) appears in DETAIL");
unlike($stderr, qr/\Q$longstr\E/,
	"full message is not passed through");

$node->stop;

done_testing();
