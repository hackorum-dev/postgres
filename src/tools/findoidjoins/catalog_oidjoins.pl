#! /usr/bin/perl

#######################################################################
#
# catalog_oidjoins.pl -- parses catalog references out of catalogs.sgml
#
# Copyright (c) 2021, PostgreSQL Global Development Group
#
# src/tools/findoidjoins/catalog_oidjoins.pl
#######################################################################

use strict;
use warnings;

use XML::Simple;
use File::Slurp qw(slurp);
use Cwd 'abs_path';
use Data::Dumper;

my $filename = shift || abs_path() . "/../../../doc/src/sgml/catalogs.sgml";

die "Usage: $0 [path to catalogs.sgml]\n" unless -e $filename;

# Need to define &mdash; entity
my $doc = <<EOF
<!DOCTYPE doc [
    <!ENTITY mdash "&#8212;" >
]>
EOF
. slurp($filename);

my $xml = new XML::Simple;
my $o = $xml->XMLin($doc);

foreach my $id (grep { /^catalog-pg-/ } sort keys %{$o->{sect1}}) {
  my $sect = $o->{sect1}->{$id};
  next unless exists $sect->{title};
  my $title = $sect->{title};
  next unless exists $title->{structname};
  my $from_rel = $title->{structname};
  next unless exists $sect->{table};
  my $table = $sect->{table};
  if (ref($table) eq 'ARRAY') {
    $table = $table->[0];
  }
  next unless exists $table->{tgroup};
  my $tgroup = $table->{tgroup};
  next unless exists $tgroup->{tbody};
  my $tbody = $tgroup->{tbody};
  next unless exists $tbody->{row};
  my $row = $tbody->{row};
  if (ref($row) eq 'HASH') {
    $row = [$row];
  }
  foreach my $r (@{$row}) {
    next unless exists $r->{entry};
    my $entry = $r->{entry};
    next unless exists $entry->{role} && $entry->{role} eq 'catalog_table_entry';
    next unless exists $entry->{para};
    my $para = $entry->{para};
    next unless ref($para) eq 'ARRAY' && scalar @{$para} == 2;
    my $p = $para->[0];
    next unless exists $p->{role} && $p->{role} eq 'column_definition';
    next unless exists $p->{link};
    my $link = $p->{link};
    next unless exists $link->{structname};
    my $to_rel = $link->{structname};
    next unless exists $p->{type};
    my $type = $p->{type};
    my $arrow = $type =~ m/\[\]$|^oidvector$/ ? "[]=>" : "=>";
    my $structfield = $p->{structfield};
    next unless ref($structfield) eq 'ARRAY' && scalar @{$structfield} == 2;
    my ($from_col, $to_col) = @{$structfield};
    if (ref($from_col) eq 'HASH'
      && exists $from_col->{replaceable}
      && $from_col->{replaceable} eq 'N'
      && $from_rel eq 'pg_statistic'
      && exists $from_col->{content}
    ) {
      my $content = $from_col->{content};
      foreach my $n (1..5) {
        print "Join pg_catalog.$from_rel.$content$n $arrow pg_catalog.$to_rel.$to_col\n";
      }
    } elsif (ref($from_col) eq '') {
      print "Join pg_catalog.$from_rel.$from_col $arrow pg_catalog.$to_rel.$to_col\n";
    } else {
      print Dumper $from_col;
    }
  }
}
