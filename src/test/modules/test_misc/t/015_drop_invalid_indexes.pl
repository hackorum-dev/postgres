use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::BackgroundPsql;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('drop_invalid_index');
$node->init;
$node->start;


# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

$node->safe_psql('postgres', q(CREATE TABLE tt (i INT PRIMARY KEY);));

$node->safe_psql(
	'postgres',
	q{
      INSERT INTO tt SELECT generate_series(1,10);
    }
);

$node->safe_psql('postgres',
	"SELECT injection_points_attach('index-build-after-am-callback', 'wait');");

my $psql1 = $node->background_psql('postgres',  wait => 0);

$psql1->query_safe(qq[SET application_name TO drop_invalid_index;]);

# This pauses on the injection point while populating catcache list
# for functions with name "foofunc"
$psql1->query_until(
	qr/starting_bg_psql/, q(
   \echo starting_bg_psql
   REINDEX TABLE CONCURRENTLY tt;
));

$node->safe_psql(
    'postgres',
	q{
      select pg_cancel_backend(pid) from pg_stat_activity where application_name = 'drop_invalid_index';
    }
);


is( $node->safe_psql(
		'postgres',
		"select count(1) from pg_index where not indisvalid;"),
	'1',
	'dropped invalid index');

$node->safe_psql(
    'postgres',
	q{
		select * from pg_drop_invalid_indexes('tt');
    }
);

is( $node->safe_psql(
		'postgres',
		"select count(1) from pg_index where not indisvalid;"),
	'0',
	'dropped invalid index');


done_testing();
