#!/usr/bin/env perl

#################################################################
#
# Perform some simple checks on on the handling of node types.
#
# Execute with the top of the PostgreSQL source tree as current
# directory.
#
# Copyright (c) 2017, PostgreSQL Global Development Group
#
#################################################################

use strict;
use warnings;

sub scan_node_categories {
  my $path = "src/include/nodes/nodes.h";
  my $category = "INVALID";
  my %results = ();
  open my $file, $path or die "Could not open $path: $!";
  while (my $line = <$file>) {
    if ($line =~ /TAGS FOR ([A-Z].*(NODES|STUFF))/) {
      $category = $1;
      $results{ $category } = [];
    } elsif ($line =~ /\W(T_[a-zA-Z0-9_]+)/) {
      push @{ $results{ $category } }, $1;
    }
  }
  return %results;
}

sub scan_switch {
  my ($path, $function) = @_;
  my @results = [];
  my $in_function = 0;
  open my $file, $path or die "Could not open $path: $!";
  while (my $line = <$file>) {
    if ($line =~ /^$function\(/) {
      $in_function = 1;
    } elsif ($line =~ /^}/) {
      $in_function = 0;
    } elsif ($in_function && $line =~ /case (T_.+):/) {
      push @results, $1;
    }
  }
  return @results;
}

sub scan_read_tagnames {
  my $path = "src/backend/nodes/readfuncs.c";
  my @results = [];
  open my $file, $path or die "Could not open $path: $!";
  while (my $line = <$file>) {
    if ($line =~ /MATCH\(\"([A-Z]+)\", ([0-9]+)/) {
      push @results, $1;
      if (length($1) != $2) {
        print "Unexpected length in readfuncs.c: $line";
      }
    }
  }
  return @results;
}

sub scan_out_tagnames {
  my $path = "src/backend/nodes/outfuncs.c";
  my %results = ();
  my $current_tag = "";
  open my $file, $path or die "Could not open $path: $!";
  while (my $line = <$file>) {
    if ($line =~ /_out.*, const ([^ ]+) \*/) {
      # see makeNode macro definition: typedef Foo must have enum T_Foo
      $current_tag = "T_" . $1;
    } elsif ($line =~ /^}/) {
      $current_tag = "";
    } elsif ($current_tag ne "" &&
      $line =~ /WRITE_NODE_TYPE\(\"([A-Z]+)\"\)/) {
      $results{$current_tag} = $1;
    }
  }
  return %results;
}

# how are you supposed to write the equivalent of python x is in <list>?
sub is_in {
  my ($needle, @haystack) = @_;
  foreach my $value (@haystack) {
    if ($needle eq $value) {
      return 1;
    }
  }
  return 0;
}

my %node_categories = scan_node_categories();

#################################################################
#
# Check node categories that must be supported by copy, out.  We'll
# exclude some special cases that don't fit the usual pattern, trusting
# that they just work.
#
#################################################################

my @copyfuncs = scan_switch("src/backend/nodes/copyfuncs.c", "copyObjectImpl");
my @outfuncs = scan_switch("src/backend/nodes/outfuncs.c", "outNode");
my @special_cases = ( "T_List", "T_IntList", "T_OidList", "T_Integer",
                      "T_Float", "T_String", "T_BitString", "T_Expr",
                      "T_Null", "T_Value" );

foreach my $category ("PLAN NODES", "PRIMITIVE NODES", "VALUE NODES", "LIST NODES", "PARSE TREE NODES") {
  foreach my $node (@{ $node_categories{ $category} }) {
    if (!is_in($node, @special_cases)) {
      if (!is_in($node, @copyfuncs)) {
        print "$node (category $category) not handled in copyfuncs.c\n";
      }
      if (!is_in($node, @outfuncs)) {
        print "$node (category $category) not handled in outfuncs.c\n";
      }
    }
  }
}

#################################################################
#
# Check node categories that must be supported by equals.
#
#################################################################

my @equalfuncs = scan_switch("src/backend/nodes/equalfuncs.c", "equal");

foreach my $category ("PRIMITIVE NODES", "VALUE NODES", "LIST NODES", "PARSE TREE NODES") {
  foreach my $node (@{ $node_categories{ $category} }) {
    if (!is_in($node, @special_cases)) {
      if (!is_in($node, @equalfuncs)) {
        print "$node (category $category) not handled in equalfuncs.c\n";
      }
    }
  }
}

#################################################################
#
# Check node categories that must be supported by parseNodeString
# using the tagname that is output by outfuncs.c
#
#################################################################

my @readfuncs_tagnames = scan_read_tagnames();
my %outfuncs_tagnames = scan_out_tagnames();

foreach my $category ("PLAN NODES", "PRIMITIVE NODES") {
  foreach my $node (@{ $node_categories{ $category} }) {
    if (exists($outfuncs_tagnames{ $node }) &&
        !is_in($outfuncs_tagnames{ $node }, @readfuncs_tagnames)) {
      print "$node is written by outfuncs.c as $outfuncs_tagnames{ $node } but that name is not recognized by readfuncs.c\n";
    }
  }
}

#################################################################
#
# Check node categories that must be supported by ruleutils.c.
#
#################################################################

my @get_rule_expr = scan_switch("src/backend/utils/adt/ruleutils.c", "get_rule_expr");

foreach my $category ("PRIMITIVE NODES") {
  foreach my $node (@{ $node_categories{ $category} }) {
    if(!is_in($node, @get_rule_expr)) {
      print "$node (category $category) not handled in ruleutils.c:get_rule_expr\n";
    }
  }
}

# TODO: Figure out what checks to run on these switch statements (and more...)

my @exprSetCollation = scan_switch("src/backend/nodes/nodeFuncs.c", "exprSetCollation");
my @expression_tree_walker = scan_switch("src/backend/nodes/nodeFuncs.c", "expression_tree_walker");
my @expression_tree_mutator = scan_switch("src/backend/nodes/nodeFuncs.c", "expression_tree_mutator");
my @raw_expression_tree_walker = scan_switch("src/backend/nodes/nodeFuncs.c", "expression_tree_mutator");
