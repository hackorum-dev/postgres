use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;


$node->safe_psql("postgres", "CREATE TABLE t0(a int)");
$node->safe_psql("postgres", "CREATE TABLE t1(a int)");
$node->safe_psql("postgres", "INSERT INTO t0 SELECT * From generate_series(1,5)");
$node->safe_psql("postgres", "INSERT INTO t1 SELECT * From generate_series(1,7)");



my %default_regexp = (
    'double_filter' => qr/^
            \QCOPY public.t0 (a) FROM stdin;\E\n
            \Q4\E\n\Q5\E\n\Q\.\E\n
            (.|\n)*
            \QCOPY public.t1 (a) FROM stdin;\E\n
            \Q4\E\n\Q5\E\n\Q6\E\n\Q7\E\n
            \Q\.\E
        /xm,
    'only_one_filter' => qr/^
            \QCOPY public.t0 (a) FROM stdin;\E\n
            \Q4\E\n\Q5\E\n
            \Q\.\E\n
        /xm,
    'one_filter' => qr/^
            \QCOPY public.t0 (a) FROM stdin;\E\n
            \Q4\E\n\Q5\E\n
            \Q\.\E\n
            (.|\n)*
            \QCOPY public.t1 (a) FROM stdin;\E\n
            \Q1\E\n\Q2\E\n\Q3\E\n\Q4\E\n\Q5\E\n\Q6\E\n\Q7\E\n
            \Q\.\E
        /xm,
    'two_filter' => qr/^
            \QCOPY public.t0 (a) FROM stdin;\E\n
            \Q1\E\n\Q2\E\n\Q3\E\n\Q4\E\n\Q5\E\n
            \Q\.\E\n
            (.|\n)*
            \QCOPY public.t1 (a) FROM stdin;\E\n
            \Q4\E\n\Q5\E\n\Q6\E\n\Q7\E\n
            \Q\.\E
        /xm,
    'revers_filter' => qr/^
            \QCOPY public.t0 (a) FROM stdin;\E\n
            \Q4\E\n\Q5\E\n
            \Q\.\E\n
            (.|\n)*
            \QCOPY public.t1 (a) FROM stdin;\E\n
            \Q1\E\n\Q2\E\n
            \Q\.\E
        /xm,
);


my %tests = (
    'gloabl filter' => {
		regexp => $default_regexp{'double_filter'},
        file => 'where a > 3',
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/gloabl filter.sql",
            '--where', 'a > 3']
            },
    'local filter' => {
		regexp => $default_regexp{'only_one_filter'},
        file => "t0 \n"
                ."t0 where a > 3",
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/local filter.sql",
            '-t', 't0',
            '--where', 'a > 3']
            },
    'local filter with second no filter table' => {
		regexp => $default_regexp{'one_filter'},
        file =>  "t0 where a > 3 \n",
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/local filter with second no filter table.sql",
            '-t', 't0',
            '--where', 'a > 3',
            '-t', 't1',
            ]
        },
    'local filter with first no filter table' => {
		regexp => $default_regexp{'two_filter'},
        file =>  "t1 where a > 3",
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/local filter with first no filter table.sql",
            '-t', 't0',
            '-t', 't1',
            '--where', 'a > 3',
            ]
        },
    
    'local filter with half name' => {
		regexp => $default_regexp{'two_filter'},
        file =>  "t1 where a > 3",
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/local filter with half name.sql",
            '--where', 't1@a > 3',
            ]
        },

    'local filter with search condition' => {
		regexp => $default_regexp{'double_filter'},
        file =>  "t0|t1 where a > 3",
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/local filter with search condition.sql",
            '-t', 't0|t1',
            '--where', 'a > 3',
            ]
        },

    'gloabal with loacal filter' => {
		regexp => $default_regexp{'revers_filter'},
        file =>  "where a > 3\n"
                ."t1 where a < 3",
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/gloabal with loacal filter.sql",
            '--where', 'a > 3',
            '-t', 't0',
            '-t', 't1',
            '--where', 'a < 3',
            ]
        },
);



foreach my $test (sort keys %tests)
{
	$node->command_ok(\@{ $tests{$test}->{dump} },"$test: pg_dump runs");

	my $output_file = slurp_file("$tempdir/${test}.sql");

    ok($output_file =~ $tests{$test}->{regexp}, "$test: should be dumped");


    open my $fileHandle, ">>", "$tempdir/${test}.filter";
    print $fileHandle $tests{$test}->{file};
    close ($fileHandle);

    $node->command_ok([
            'pg_dump',
            'postgres',
            '-f', "$tempdir/${test} with file.sql",
            '--file-filter', "$tempdir/${test}.filter",
            ],
            "$test: pg_dump with config runs");

   	my $output_file_f = slurp_file("$tempdir/${test} with file.sql");
    ok($output_file_f =~ $tests{$test}->{regexp}, "$test with file: should be dumped");
}

done_testing();
