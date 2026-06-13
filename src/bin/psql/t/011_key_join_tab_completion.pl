# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Data::Dumper;

# Do nothing unless Makefile has told us that the build is --with-readline.
if (!defined($ENV{with_readline}) || $ENV{with_readline} ne 'yes')
{
	plan skip_all => 'readline is not supported by this build';
}

# Also, skip if user has set environment variable to command that.
# This is mainly intended to allow working around some of the more broken
# versions of libedit --- some users might find them acceptable even if
# they won't pass these tests.
if (defined($ENV{SKIP_READLINE_TESTS}))
{
	plan skip_all => 'SKIP_READLINE_TESTS is set';
}

# If we don't have IO::Pty, forget it, because IPC::Run depends on that
# to support pty connections.
eval { require IO::Pty; };
if ($@)
{
	plan skip_all => 'IO::Pty is needed to run this test';
}

# Start a new server.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Set up relations with enough foreign keys to exercise FOR KEY completion.
$node->safe_psql('postgres',
		"CREATE TABLE key_join_parent (id int PRIMARY KEY);\n"
	  . "CREATE TABLE key_join_child"
	  . " (parent_id int REFERENCES key_join_parent(id));\n"
	  . "CREATE TABLE key_join_customers (id int PRIMARY KEY);\n"
	  . "CREATE TABLE key_join_orders (id int PRIMARY KEY,"
	  . " customer_id int REFERENCES key_join_customers(id));\n"
	  . "CREATE TABLE key_join_order_items (id int PRIMARY KEY,"
	  . " order_id int REFERENCES key_join_orders(id));\n"
	  . "CREATE TABLE key_join_departments (dept_id int PRIMARY KEY);\n"
	  . "CREATE TABLE key_join_employees (emp_id int PRIMARY KEY,"
	  . " dept_id int REFERENCES key_join_departments(dept_id),"
	  . " home_dept int REFERENCES key_join_departments(dept_id));\n"
	  . "CREATE TABLE key_join_named_departments (dept_id int"
	  . " PRIMARY KEY, name text UNIQUE, active boolean DEFAULT true);\n"
	  . "CREATE TABLE key_join_named_employees (emp_id int PRIMARY KEY,"
	  . " dept_id int REFERENCES key_join_named_departments(dept_id),"
	  . " home_dept int REFERENCES key_join_named_departments(dept_id),"
	  . " dept_name text REFERENCES key_join_named_departments(name));\n"
	  . "CREATE TABLE key_join_no_fk_left (id int PRIMARY KEY);\n"
	  . "CREATE TABLE key_join_no_fk_right (id int PRIMARY KEY);\n"
	  . "CREATE TABLE key_join_staff (staff_id int PRIMARY KEY,"
	  . " boss_id int REFERENCES key_join_staff(staff_id));\n"
	  . "CREATE TABLE key_join_users (user_id int, org_id int,"
	  . " PRIMARY KEY (org_id, user_id));\n"
	  . "CREATE TABLE key_join_posts (author_id int, org_id int,"
	  . " FOREIGN KEY (org_id, author_id)"
	  . " REFERENCES key_join_users (org_id, user_id));\n"
	  . "CREATE SCHEMA key_join_schema;\n"
	  . "CREATE TABLE key_join_schema.\"Key Join Referenced\""
	  . " (\"Ref ID\" int PRIMARY KEY);\n"
	  . "CREATE TABLE key_join_schema.\"Key Join Referencing\""
	  . " (\"Ref ID\" int REFERENCES"
	  . " key_join_schema.\"Key Join Referenced\"(\"Ref ID\"));\n"
	  . "CREATE TABLE key_join_q_ref (\"r)id\" int PRIMARY KEY);\n"
	  . "CREATE TABLE key_join_q_fk (\"a)x\" int"
	  . " REFERENCES key_join_q_ref(\"r)id\"),"
	  . " \"b)x\" int REFERENCES key_join_q_ref(\"r)id\"));\n"
	  . "CREATE TABLE kjp (id int PRIMARY KEY);\n"
	  . "CREATE TABLE kjc (p int REFERENCES kjp(id));\n");

# Arrange to capture, not discard, the interactive session's history output.
# Put it in the test log directory, so that buildfarm runs capture the result
# for possible debugging purposes.
my $historyfile = "${PostgreSQL::Test::Utils::log_path}/011_psql_history.txt";

# Fire up an interactive psql session and configure it such that each query
# restarts the timer.
my $h = $node->interactive_psql('postgres', history_file => $historyfile);
$h->set_query_timer_restart();

sub check_completion
{
	my ($send, $pattern, $annotation) = @_;

	# report test failures from caller location
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	# send the data to be sent and wait for its result
	my $out = $h->query_until($pattern, $send);
	my $okay = ($out =~ $pattern && !$h->{timeout}->is_expired);
	ok($okay, $annotation);
	# for debugging, log actual output if it didn't match
	local $Data::Dumper::Terse = 1;
	local $Data::Dumper::Useqq = 1;
	diag 'Actual output was ' . Dumper($out) . "Did not match \"$pattern\"\n"
	  if !$okay;
	return;
}

# Clear query buffer to start over
# (won't work if we are inside a string literal!)
sub clear_query
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	check_completion("\\r\n", qr/Query buffer reset.*postgres=# /s,
		"\\r works");
	return;
}

# Like check_completion, but expect the output NOT to match the pattern;
# the line is then abandoned with control-U
sub check_no_completion_match
{
	my ($send, $pattern, $annotation) = @_;

	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $out = $h->query_until(qr/postgres=# /s, $send . "\025\n");
	my $okay = ($out !~ $pattern && !$h->{timeout}->is_expired);
	ok($okay, $annotation);
	local $Data::Dumper::Terse = 1;
	local $Data::Dumper::Useqq = 1;
	diag 'Actual output was '
	  . Dumper($out)
	  . "Unexpectedly matched \"$pattern\"\n"
	  if !$okay;
	return;
}

# check tab completion for FOR KEY joins

# The test pty is 80 columns wide and readline scrolls longer lines
# horizontally, so once a completed line exceeds the width only its last
# few dozen characters remain in the captured output.  Cases that need to
# see the start of the completion keep the line short (the kjp/kjc tables
# exist for that); the others match only the tail of the clause.

# When more than one foreign key matches the typed prefix, completion
# advances one phrase at a time: a phrase shared by every matching clause
# completes by itself, and the menu then shows only the next, diverging
# phrase of each clause.
# SELECT * FROM key_join_employees LEFT JOIN key_join_departments f\t
# SELECT * FROM key_join_employees LEFT JOIN key_join_departments
#   for key key_join_employees (
check_completion(
	"SELECT * FROM key_join_employees LEFT JOIN key_join_departments f\t",
	qr/f\x07?or key key_join_employees \( (?!dept_id|home_dept)/i,
	"complete shared FOR KEY left reference phrase of ambiguous join");

# \t\t -> only the diverging referencing columns appear in the menu
check_completion("\t\t", qr/dept_id \) +home_dept \)/,
	"offer only the diverging phrase of ambiguous join");

# h\t -> the single matching clause completes in full
check_completion("h\t",
	qr/home_dept \) -> key_join_departments \( dept_id \)/,
	"complete one clause after disambiguating menu");

clear_query();

# SELECT * FROM key_join_employees LEFT JOIN key_join_departments
#   for key key_join_employees ( \t\t
# dept_id )  home_dept )
check_completion(
	"SELECT * FROM key_join_employees LEFT JOIN key_join_departments "
	  . "for key key_join_employees ( \t\t",
	qr/dept_id \) +home_dept \)/,
	"offer suffix alternatives from canonical typed prefix");

clear_query();

# SELECT * FROM kjp JOIN kjc f\t
# SELECT * FROM kjp JOIN kjc for key kjp ( id ) <- kjc ( p )
check_completion(
	"SELECT * FROM kjp JOIN kjc f\t",
	qr/f\x07?or key kjp \( id \) <- kjc \( p \)/i,
	"complete unique FOR KEY join from join-condition prefix");

clear_query();

# SELECT * FROM kjp JOIN kjc FOR K\t
# SELECT * FROM kjp JOIN kjc FOR KEY kjp ( id ) <- kjc ( p )
check_completion(
	"SELECT * FROM kjp JOIN kjc FOR K\t",
	qr/K\x07?EY kjp \( id \) <- kjc \( p \)/,
	"complete unique FOR KEY join after FOR K");

clear_query();

# SELECT * FROM kjp JOIN kjc FOR \t
# SELECT * FROM kjp JOIN kjc FOR KEY kjp ( id ) <- kjc ( p )
check_completion(
	"SELECT * FROM kjp JOIN kjc FOR \t",
	qr/KEY kjp \( id \) <- kjc \( p \)/,
	"complete unique FOR KEY join after FOR");

clear_query();

# SELECT * FROM kjp JOIN kjc for \t
# SELECT * FROM kjp JOIN kjc for key kjp ( id ) <- kjc ( p )
check_completion(
	"SELECT * FROM kjp JOIN kjc for \t",
	qr/key kjp \( id \) <- kjc \( p \)/,
	"complete unique FOR KEY join preserving lowercase FOR");

clear_query();

# SELECT * FROM kjp JOIN kjc for k\t
# SELECT * FROM kjp JOIN kjc for key kjp ( id ) <- kjc ( p )
check_completion(
	"SELECT * FROM kjp JOIN kjc for k\t",
	qr/k\x07?ey kjp \( id \) <- kjc \( p \)/,
	"complete unique FOR KEY join after lowercase for k");

clear_query();

# SELECT * FROM kjp JOIN kjc AS c FOR \t
# SELECT * FROM kjp JOIN kjc AS c FOR KEY kjp ( id ) <- c ( p )
check_completion(
	"SELECT * FROM kjp JOIN kjc AS c FOR \t",
	qr/KEY kjp \( id \) <- c \( p \)/,
	"complete FOR KEY join spelling the joined relation by its alias");

clear_query();

# SELECT * FROM key_join_orders JOIN key_join_customers \t\t
# FOR KEY  ON  USING (
check_completion(
	"SELECT * FROM key_join_orders JOIN key_join_customers \t\t",
	qr/(?=.*FOR KEY)(?=.*ON)(?=.*USING \()/s,
	"offer ordinary join conditions including FOR KEY");

clear_query();

# SELECT * FROM key_join_no_fk_left JOIN key_join_no_fk_right FOR \t
# SELECT * FROM key_join_no_fk_left JOIN key_join_no_fk_right FOR
check_no_completion_match(
	"SELECT * FROM key_join_no_fk_left JOIN key_join_no_fk_right FOR \t",
	qr/KEY key_join_no_fk/,
	"do not infer FOR KEY join after FOR without foreign key");

clear_query();

# SELECT * FROM key_join_no_fk_left JOIN key_join_no_fk_right f\t
# SELECT * FROM key_join_no_fk_left JOIN key_join_no_fk_right for key
check_completion(
	"SELECT * FROM key_join_no_fk_left JOIN key_join_no_fk_right f\t",
	qr/f\x07?or key (?!key_join)/i,
	"complete ordinary FOR KEY prefix without foreign key inference");

clear_query();

# SELECT * FROM key_join_order_items i JOIN key_join_orders o
#   FOR KEY i ( order_id ) -> o ( id ) JOIN key_join_customers c FOR \t
# SELECT * FROM key_join_order_items i JOIN key_join_orders o
#   FOR KEY i ( order_id ) -> o ( id ) JOIN key_join_customers c
#   FOR KEY o ( customer_id ) -> c ( id )
check_completion(
	"SELECT * FROM key_join_order_items i JOIN key_join_orders o "
	  . "FOR KEY i ( order_id ) -> o ( id ) "
	  . "JOIN key_join_customers c FOR \t",
	qr/KEY o \( customer_id \) -> c \( id \)/,
	"complete FOR KEY join using visible alias");

clear_query();

# SELECT * FROM key_join_orders AS o JOIN key_join_customers c FOR \t
# SELECT * FROM key_join_orders AS o JOIN key_join_customers c
#   FOR KEY o ( customer_id ) -> c ( id )
check_completion(
	"SELECT * FROM key_join_orders AS o JOIN key_join_customers c FOR \t",
	qr/KEY o \( customer_id \) -> c \( id \)/,
	"complete FOR KEY join preserving AS alias");

clear_query();

# SELECT * FROM key_join_orders key JOIN key_join_customers c FOR \t
# SELECT * FROM key_join_orders key JOIN key_join_customers c
#   FOR KEY key ( customer_id ) -> c ( id )
check_completion(
	"SELECT * FROM key_join_orders key JOIN key_join_customers c FOR \t",
	qr/KEY key \( customer_id \) -> c \( id \)/,
	"complete FOR KEY join preserving alias named key");

clear_query();

# SELECT * FROM key_join_parent JOIN key_join_child FOR \t
# SELECT * FROM key_join_parent JOIN key_join_child
#   FOR KEY key_join_parent ( id ) <- key_join_child ( parent_id )
check_completion(
	"SELECT * FROM key_join_parent JOIN key_join_child FOR \t",
	qr/<- key_join_child \( parent_id \)/,
	"complete FOR KEY join when right side has foreign key");

clear_query();

# SELECT * FROM key_join_named_employees e JOIN key_join_named_departments d
#   FOR KEY e ( dept_name ) -> d ( \t
# SELECT * FROM key_join_named_employees e JOIN key_join_named_departments d
#   FOR KEY e ( dept_name ) -> d ( name )
check_completion(
	"SELECT * FROM key_join_named_employees e "
	  . "JOIN key_join_named_departments d FOR KEY e ( dept_name ) -> d ( \t",
	qr/-> d \( name \)/,
	"complete unique suffix from canonical typed prefix");

clear_query();

# The key columns of the multicolumn foreign key are deliberately not in
# attnum order, so this also verifies that columns appear in key order.
# SELECT * FROM key_join_posts p JOIN key_join_users u FOR KEY p ( org_id, a\t
# SELECT * FROM key_join_posts p JOIN key_join_users u
#   FOR KEY p ( org_id, author_id ) -> u ( org_id, user_id )
check_completion(
	"SELECT * FROM key_join_posts p JOIN key_join_users u "
	  . "FOR KEY p ( org_id, a\t",
	qr/author_id \) -> u \( org_id, user_id \)/,
	"complete multicolumn FOR KEY join from canonical typed prefix");

clear_query();

# A self-referencing foreign key in a self-join can be used in either
# direction.  The KEY s1 ( phrase is shared by both directions; the menu
# then offers only their key columns.
# SELECT * FROM key_join_staff s1 JOIN key_join_staff s2 FOR \t
# SELECT * FROM key_join_staff s1 JOIN key_join_staff s2 FOR KEY s1 (
check_completion(
	"SELECT * FROM key_join_staff s1 JOIN key_join_staff s2 FOR \t",
	qr/FOR KEY s1 \(/,
	"complete shared KEY s1 ( phrase of self-referencing foreign key");

# \t\t -> menu with the key columns of both directions
check_completion("\t\t", qr/boss_id \) +staff_id \)/,
	"offer both directions for self-referencing foreign key");

# s\t -> the single matching direction completes in full
check_completion("s\t", qr/staff_id \) <- s2 \( boss_id \)/,
	"complete one direction after disambiguating menu");

clear_query();

# SELECT * FROM key_join_staff s1 JOIN key_join_staff s2 FOR KEY s1 ( s\t
# SELECT * FROM key_join_staff s1 JOIN key_join_staff s2
#   FOR KEY s1 ( staff_id ) <- s2 ( boss_id )
check_completion(
	"SELECT * FROM key_join_staff s1 JOIN key_join_staff s2 "
	  . "FOR KEY s1 ( s\t",
	qr/staff_id \) <- s2 \( boss_id \)/,
	"complete one direction of self-referencing FOR KEY join");

clear_query();

# SELECT * FROM key_join_schema."Key Join Referencing"
#   JOIN key_join_schema."Key Join Referenced" FOR \t
# SELECT * FROM key_join_schema."Key Join Referencing"
#   JOIN key_join_schema."Key Join Referenced"
#   FOR KEY "Key Join Referencing" ( "Ref ID" ) -> "Key Join Referenced" ( "Ref ID" )
check_completion(
	"SELECT * FROM key_join_schema.\"Key Join Referencing\" "
	  . "JOIN key_join_schema.\"Key Join Referenced\" FOR \t",
	qr/-> "Key Join Referenced" \( "Ref ID" \)/,
	"complete FOR KEY join with quoted schema-qualified identifiers");

clear_query();

# Phrase truncation must not split a quoted identifier containing a
# parenthesis: the shared "r)id" ) phrase completes whole, and the menu
# offers the quoted referencing columns.
# SELECT * FROM key_join_q_ref r JOIN key_join_q_fk f for key r ( \t
# SELECT * FROM key_join_q_ref r JOIN key_join_q_fk f for key r ( "r)id" )
check_completion(
	"SELECT * FROM key_join_q_ref r JOIN key_join_q_fk f for key r ( \t",
	qr/for key r \( "r\)id" \)/,
	"complete shared phrase without splitting quoted parenthesis");

# \t -> <- f ( ; the direction phrase is shared as well
check_completion("\t", qr/<- f \(/,
	"complete shared direction phrase with quoted key columns");

# \t -> " ; the quote is the longest common prefix of the alternatives
check_completion("\t", qr/"/,
	"insert common quote of diverging quoted columns");

# \t\t -> menu with the two quoted referencing columns
check_completion("\t\t", qr/"a\)x" \) +"b\)x" \)/,
	"offer quoted referencing columns in menu");

# a\t -> the rest of the single matching clause, balancing the quotes
check_completion("a\t", qr/a\x07?\)x" \) /,
	"complete quoted column after disambiguation");

clear_query();

# Send psql an explicit \q to shut it down, else pty won't close properly.
$h->quit or die "psql returned $?";

$node->stop;
done_testing();
