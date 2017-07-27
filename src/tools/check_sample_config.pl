#!/usr/bin/env perl

#################################################################
#
# Check for discrepancies between guc.c and postgresql.conf.sample.
#
# Execute with the top of the PostgreSQL source tree as current
# directory.
#
# Copyright (c) 2017, PostgreSQL Global Development Group
#
#################################################################

use strict;
use warnings;

sub scan_sample_config {
  my $path = "src/backend/utils/misc/postgresql.conf.sample";
  my @result = ();
  open my $file, $path or die "Could not open $path: $!";
  while (my $line = <$file>) {
    if ($line =~ /^#([^ ]+) = /) {
      push @result, lc $1;
    }
  }
  close $file;
  return @result;
}

sub scan_gucs {
  my $path = "src/backend/utils/misc/guc.c";
  my @result = ();
  my $guc = "";
  open my $file, $path or die "Could not open $path: $!";
  while (my $line = <$file>) {
    if ($line =~ /^		\{ *"([^"]+)",/) {
      $guc = $1;
    } elsif ($line =~ /GUC_NOT_IN_SAMPLE/) {
      $guc = "";
    } elsif ($line =~ /^\W\},$/) {
      if ($guc ne "") {
        push @result, lc $guc;
      }
    }
  }
  close $file;
  return @result;
}

sub is_in {
  my ($needle, @haystack) = @_;
  foreach my $value (@haystack) {
    if ($needle eq $value) {
      return 1;
    }
  }
  return 0;
}

my @sample_config = scan_sample_config();
my @gucs = scan_gucs();
my @ignore = ("include", "include_if_exists", "include_dir", "config_file");
my $fail = 0;

foreach my $guc (@sample_config) {
  if (!is_in($guc, @gucs) && !is_in($guc, @ignore)) {
    printf "$guc appears in postgresql.conf.sample but not in guc.c\n";
    $fail = 1;
  }
}

foreach my $guc (@gucs) {
  if (!is_in($guc, @sample_config) && !is_in($guc, @ignore)) {
    printf "$guc appears in guc.c but not in postgresql.conf.sample\n";
    $fail = 1;
  }
}

exit 1 if $fail;
