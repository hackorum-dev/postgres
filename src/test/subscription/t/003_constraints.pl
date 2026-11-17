
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# This test checks that constraints work on subscriber
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Setup structure on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_fk (bid int PRIMARY KEY);");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_fk_ref (id int PRIMARY KEY, junk text, bid int REFERENCES tab_fk (bid));"
);

# Setup structure on subscriber; column order intentionally different
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_fk (bid int PRIMARY KEY);");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_fk_ref (id int PRIMARY KEY, bid int REFERENCES tab_fk (bid), junk text);"
);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR ALL TABLES;");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub WITH (copy_data = false)"
);

$node_publisher->wait_for_catchup('tap_sub');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk (bid) VALUES (1);");
# "junk" value is meant to be large enough to force out-of-line storage
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid, junk) VALUES (1, 1, repeat(pi()::text,20000));"
);

$node_publisher->wait_for_catchup('tap_sub');

# Check data on subscriber
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk;");
is($result, qq(1|1|1), 'check replicated tab_fk inserts on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(1|1|1), 'check replicated tab_fk_ref inserts on subscriber');

# Drop the fk on publisher
$node_publisher->safe_psql('postgres', "DROP TABLE tab_fk CASCADE;");

# Insert data
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid) VALUES (2, 2);");

$node_publisher->wait_for_catchup('tap_sub');

# FK is not enforced on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2), 'check FK ignored on subscriber');

# Add replica trigger
$node_subscriber->safe_psql(
	'postgres', qq{
CREATE FUNCTION filter_basic_dml_fn() RETURNS TRIGGER AS \$\$
BEGIN
    IF (TG_OP = 'INSERT') THEN
        IF (NEW.id < 10) THEN
            RETURN NEW;
        ELSE
            RETURN NULL;
        END IF;
    ELSIF (TG_OP = 'UPDATE') THEN
        RETURN NULL;
    ELSE
        RAISE WARNING 'Unknown action';
        RETURN NULL;
    END IF;
END;
\$\$ LANGUAGE plpgsql;
CREATE TRIGGER filter_basic_dml_trg
    BEFORE INSERT OR UPDATE OF bid ON tab_fk_ref
    FOR EACH ROW EXECUTE PROCEDURE filter_basic_dml_fn();
ALTER TABLE tab_fk_ref ENABLE REPLICA TRIGGER filter_basic_dml_trg;
});

# Insert data
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid) VALUES (10, 10);");

$node_publisher->wait_for_catchup('tap_sub');

# The trigger should cause the insert to be skipped on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2), 'check replica insert trigger applied on subscriber');

# Update data
$node_publisher->safe_psql('postgres',
	"UPDATE tab_fk_ref SET bid = 2 WHERE bid = 1;");

$node_publisher->wait_for_catchup('tap_sub');

# The trigger should cause the update to be skipped on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2),
	'check replica update column trigger applied on subscriber');

# Update on a column not specified in the trigger, but it will trigger
# anyway because logical replication ships all columns in an update.
$node_publisher->safe_psql('postgres',
	"UPDATE tab_fk_ref SET id = 6 WHERE id = 1;");

$node_publisher->wait_for_catchup('tap_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(id), max(id) FROM tab_fk_ref;");
is($result, qq(2|1|2),
	'check column trigger applied even on update for other column');

# Domain constraints must also be checked for incoming NULLs.  Use different
# types on the two nodes so that invalid values can originate on the publisher.
$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION tap_sub');
$node_publisher->safe_psql(
	'postgres', qq{
	CREATE TABLE domain_test (
		id integer PRIMARY KEY, quantity integer, nullable integer,
		plain integer, payload text);
	ALTER TABLE domain_test ALTER payload SET STORAGE EXTERNAL;
	CREATE PUBLICATION domain_pub FOR TABLE domain_test;
});
$node_subscriber->safe_psql(
	'postgres', qq{
	CREATE DOMAIN required_int AS integer NOT NULL
		CONSTRAINT required_int_positive CHECK (VALUE > 0);
	CREATE DOMAIN checked_int AS integer
		CONSTRAINT checked_int_nonnull CHECK (VALUE IS NOT NULL)
		CONSTRAINT checked_int_positive CHECK (VALUE > 0);
	CREATE DOMAIN nullable_int AS integer CHECK (VALUE > 0);
	CREATE DOMAIN required_text AS text NOT NULL;
	CREATE TABLE domain_test (
		id integer PRIMARY KEY, quantity required_int, nullable nullable_int,
		plain integer, payload required_text,
		local_only required_int DEFAULT 99);
	ALTER TABLE domain_test ALTER payload SET STORAGE EXTERNAL;
});

my $domain_query = qq{
	SELECT id, quantity, nullable IS NULL, plain IS NULL,
		length(payload), local_only FROM domain_test ORDER BY id
};

for my $binary ('false', 'true')
{
	for my $domain ('required_int', 'checked_int')
	{
		for my $operation ('insert', 'update', 'update_key', 'check')
		{
			my $testname = "binary=$binary, $domain, $operation";

			# Start each case with a fresh subscription, so a failed transaction
			# does not prevent subsequent cases from making progress.
			$node_publisher->safe_psql(
				'postgres', qq{
				TRUNCATE domain_test;
				INSERT INTO domain_test VALUES
					(1, 10, NULL, NULL, repeat('abcdefghijklmnopqrstuvwxyz', 1000));
			});
			$node_subscriber->safe_psql(
				'postgres', qq{
				TRUNCATE domain_test;
				ALTER TABLE domain_test ALTER quantity TYPE $domain
					USING quantity::integer;
			});
			$node_subscriber->safe_psql(
				'postgres', qq{
				CREATE SUBSCRIPTION domain_sub CONNECTION '$publisher_connstr'
					PUBLICATION domain_pub
					WITH (binary = $binary, disable_on_error = true)
			});
			$node_subscriber->wait_for_subscription_sync($node_publisher,
				'domain_sub');
			is($node_subscriber->safe_psql('postgres', $domain_query),
				'1|10|t|t|26000|99', "$testname: initial copy");

			if ($operation eq 'insert')
			{
				# NULL remains valid for ordinary columns and nullable domains.
				# The subscriber-only domain column must receive its default.
				$node_publisher->safe_psql(
					'postgres', qq{
					INSERT INTO domain_test VALUES
						(2, 10, NULL, NULL, repeat('abcdefghijklmnopqrstuvwxyz', 1000));
				});
				$node_publisher->wait_for_catchup('domain_sub');
				is($node_subscriber->safe_psql('postgres', $domain_query),
					"1|10|t|t|26000|99\n2|10|t|t|26000|99",
					"$testname: NULLs and subscriber-only default on insert");

				# A key update or delete sends NULL placeholders for non-key
				# columns in the old key tuple.  They must not be checked against
				# the domains.  An unchanged TOAST value is not a NULL either.
				$node_publisher->safe_psql(
					'postgres', qq{
					UPDATE domain_test SET id = 11, nullable = 1, plain = 1 WHERE id = 1;
					DELETE FROM domain_test WHERE id = 2;
				});
				$node_publisher->wait_for_catchup('domain_sub');
				is($node_subscriber->safe_psql('postgres', $domain_query),
					'11|10|f|f|26000|99',
					"$testname: key update, delete and unchanged TOAST");

				$node_publisher->safe_psql(
					'postgres', qq{
					UPDATE domain_test SET nullable = NULL, plain = NULL WHERE id = 11;
					UPDATE domain_test SET id = 1 WHERE id = 11;
				});
				$node_publisher->wait_for_catchup('domain_sub');
				is($node_subscriber->safe_psql('postgres', $domain_query),
					'1|10|t|t|26000|99',
					"$testname: nullable columns and unchanged TOAST on update");
			}

			my %changes = (
				insert => "INSERT INTO domain_test VALUES (2, NULL, NULL, NULL, 'payload')",
				update => 'UPDATE domain_test SET quantity = NULL WHERE id = 1',
				update_key => 'UPDATE domain_test SET id = 2, quantity = NULL WHERE id = 1',
				check => 'UPDATE domain_test SET quantity = -1 WHERE id = 1');
			my $error = $operation eq 'check'
				? qq(value for domain public.$domain violates check constraint "${domain}_positive")
				: $domain eq 'required_int'
				? 'domain public.required_int does not allow null values'
				: 'value for domain public.checked_int violates check constraint "checked_int_nonnull"';
			my $message = $operation eq 'insert' ? 'INSERT' : 'UPDATE';
			my $offset = -s $node_subscriber->logfile;

			# The preceding valid update must roll back along with the invalid
			# change.  Updating the key exercises replacement of the new tuple
			# after looking up the row with the old key tuple.
			$node_publisher->safe_psql(
				'postgres', qq{
				BEGIN;
				UPDATE domain_test SET plain = 7 WHERE id = 1;
				$changes{$operation};
				COMMIT;
			});
			ok($node_subscriber->poll_query_until(
					'postgres', qq{
					SELECT NOT subenabled FROM pg_subscription
						WHERE subname = 'domain_sub'
				}),
				"$testname: domain violation disables subscription");
			my $log = slurp_file($node_subscriber->logfile, $offset);
			like($log, qr/\Q$error\E/, "$testname: domain error");
			like($log,
				qr/CONTEXT:.*message type "$message".*relation "public.domain_test" column "quantity"/,
				"$testname: error identifies the target column");
			is($node_subscriber->safe_psql('postgres', $domain_query),
				'1|10|t|t|26000|99', "$testname: transaction rolled back");
			$node_subscriber->safe_psql('postgres',
				'DROP SUBSCRIPTION domain_sub');
		}
	}
}

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
