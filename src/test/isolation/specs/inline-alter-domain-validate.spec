# Test inline permutation steps from alter-domain-validate.spec

setup
{
	CREATE DOMAIN alter_domain_validate_d AS int;
	CREATE TABLE alter_domain_validate_t (a alter_domain_validate_d);
}

teardown
{
	DROP TABLE alter_domain_validate_t;
	DROP DOMAIN alter_domain_validate_d;
}

session s1
session s2
session s3

permutation
  s1 { DO $$ BEGIN PERFORM pg_advisory_lock(8888); END $$; }
  # CoerceToDomain initializes the domain constraint list during executor
  # startup, before this CTE waits on the advisory lock.
  s2 { WITH wait AS MATERIALIZED (SELECT pg_advisory_lock(8888)) INSERT INTO alter_domain_validate_t SELECT (-1)::alter_domain_validate_d FROM wait; }
  s3 { ALTER DOMAIN alter_domain_validate_d ADD CONSTRAINT alter_domain_validate_d_pos CHECK (VALUE > 0) NOT VALID; }
  s3 { ALTER DOMAIN alter_domain_validate_d VALIDATE CONSTRAINT alter_domain_validate_d_pos; }
  s1 { DO $$ BEGIN PERFORM pg_advisory_unlock(8888); END $$; }
  s3 { SELECT convalidated FROM pg_constraint WHERE conname = 'alter_domain_validate_d_pos'; }
  s3 { SELECT count(*) FROM alter_domain_validate_t; }
