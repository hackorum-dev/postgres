--
-- Regression tests for disk quota
--

CREATE SCHEMA test_ns_disk_quota

       CREATE TABLE quota_1 (
              a int,
              b int
       );

CREATE TABLE quota_2 (
        a                       int2,
        b                       float4
);

CREATE ROLE test_user_quota_1;

CREATE DISK QUOTA abc ON TABLE quota_2 WITH ();

CREATE DISK QUOTA abc ON TABLE quota_2 WITH (abc='100MB');

CREATE DISK QUOTA abc ON TABLE quota_2 WITH (quota='100MB', abc='100MB');

CREATE DISK QUOTA abc ON TABLE quota_2 WITH (quota='100MB', quota='200MB');

CREATE DISK QUOTA abc ON TABLE quota_2 WITH (quota='100MB');

CREATE DISK QUOTA abc ON TABLE quota_2 WITH (quota='100MB');

CREATE DISK QUOTA abc_error ON TABLE quota_1 WITH (quota='100MB');

CREATE DISK QUOTA abc_error ON SCHEMA quota_1 WITH (quota='100MB');

CREATE DISK QUOTA abc_error ON USER quota_1 WITH (quota='100MB');

CREATE DISK QUOTA abc ON SCHEMA test_ns_disk_quota WITH (quota='100MB');

CREATE DISK QUOTA abc_1 ON SCHEMA test_ns_disk_quota WITH (quota='100MB');

CREATE DISK QUOTA abc_2 ON TABLE test_ns_disk_quota.quota_1 WITH (quota='100MB');

CREATE DISK QUOTA abc_3 ON USER test_user_quota_1 WITH (quota='100MB');


DROP DISK QUOTA abc;
DROP DISK QUOTA abc_1;
DROP DISK QUOTA abc_2;
DROP DISK QUOTA abc_3;
DROP DISK QUOTA abc_no;

DROP SCHEMA test_ns_disk_quota CASCADE;
DROP TABLE quota_2;
DROP ROLE test_user_quota_1;
