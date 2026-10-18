# Copyright (c) 2026, PostgreSQL Global Development Group

# ILIKE in a single-byte encoding with a C ctype matches via C_IMatchText
# (like.c), which folds bytes with pg_ascii_tolower().  A '%' followed by a
# non-ASCII byte must still find it.  The main regression database is
# usually UTF-8, which never takes this path, so use a LATIN1 database.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('ilike_single_byte');
$node->init;
$node->start;

$node->safe_psql('postgres',
	q{CREATE DATABASE latin1 TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C'});

# Build every string from bytes, so the test does not depend on the client
# encoding: \xc6 is LATIN1 'Æ', \xe6 is 'æ', \xe9 is 'é'.
my $result = $node->safe_psql(
	'latin1', q{
SELECT string_agg(r::text, ',' ORDER BY n)
  FROM (VALUES
    (1, convert_from('\x78c679', 'LATIN1') ILIKE convert_from('\x25c625', 'LATIN1')),
    (2, convert_from('\x6162c6', 'LATIN1') ILIKE convert_from('\x25c6', 'LATIN1')),
    (3, convert_from('\x786579', 'LATIN1') ILIKE convert_from('\x25e925', 'LATIN1')),
    (4, convert_from('\x78c6e679', 'LATIN1') ILIKE convert_from('\x25e625', 'LATIN1')),
    (5, convert_from('\xc66364', 'LATIN1') ILIKE convert_from('\xc625', 'LATIN1')),
    (6, convert_from('\x78c679', 'LATIN1') NOT ILIKE convert_from('\x25c625', 'LATIN1')),
    (7, convert_from('\x78e679', 'LATIN1') ILIKE convert_from('\x25c625', 'LATIN1'))
  ) AS v(n, r)});

# 1: '%Æ%' finds Æ mid-string; 2: at the end; 3: '%é%' does not match 'xey';
# 4: '%æ%' finds æ after Æ; 5: no leading '%' (always worked);
# 6: the NOT ILIKE counterpart of 1; 7: Æ and æ are different bytes and the
# C ctype folds ASCII only, so '%Æ%' does not match 'xæy'.
is($result, 'true,true,false,true,true,false,false',
	'ILIKE with % before a non-ASCII byte, LATIN1 / C');

done_testing();
