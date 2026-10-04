# Test that publication DDL that widens the set of changes tables publish
# (adding TABLES IN SCHEMA or ALL TABLES, enabling publication of UPDATE or
# DELETE) serializes with concurrent data-modifying statements: the DDL must
# wait for in-progress writers, and writers must not pass the replica identity
# check with the old publication definition while the DDL commits underneath
# them.
#
# The interlocks are verified via the lock waits they produce.  The
# correctness of the outcome is verified by the replica identity error that a
# writer on a table without a replica identity gets once the DDL has
# committed.

setup
{
	CREATE SCHEMA sch1;
	CREATE TABLE sch1.t_nori (id int, val int);
	INSERT INTO sch1.t_nori VALUES (1, 1);
	CREATE TABLE sch1.t_ri (id int PRIMARY KEY, val int);
	INSERT INTO sch1.t_ri VALUES (1, 1);
	CREATE TABLE t_outside (id int, val int);
	INSERT INTO t_outside VALUES (1, 1);
	CREATE SCHEMA sch2;
	CREATE TABLE sch2.t_ins (id int, val int);
	INSERT INTO sch2.t_ins VALUES (1, 1);
	CREATE SCHEMA sch3;
	CREATE TABLE sch3.t_w (id int, val int);
	INSERT INTO sch3.t_w VALUES (1, 1);
	CREATE SCHEMA schp;
	CREATE SCHEMA schleaf;
	CREATE TABLE schp.part (id int, val int) PARTITION BY RANGE (id);
	CREATE TABLE schleaf.leaf1 PARTITION OF schp.part
		FOR VALUES FROM (0) TO (1000);
	INSERT INTO schleaf.leaf1 VALUES (1, 1);
	CREATE TABLE t_attach (id int, val int);
	INSERT INTO t_attach VALUES (1500, 1);
	CREATE PUBLICATION pub1;
	CREATE PUBLICATION pub_emp;
	CREATE SCHEMA sch_emp;
	CREATE PUBLICATION pub_ins WITH (publish = 'insert');
	CREATE PUBLICATION pub_w FOR TABLES IN SCHEMA sch3 WITH (publish = 'insert');
	CREATE PUBLICATION pub_setall;
	CREATE PUBLICATION pub_flip;
	CREATE PUBLICATION pub_part FOR TABLE schp.part;
}

teardown
{
	DROP TABLE IF EXISTS t_attach;
	DROP SCHEMA sch1 CASCADE;
	DROP SCHEMA sch2 CASCADE;
	DROP SCHEMA sch3 CASCADE;
	DROP SCHEMA schp CASCADE;
	DROP SCHEMA schleaf CASCADE;
	DROP TABLE t_outside;
	DROP PUBLICATION pub1;
	DROP PUBLICATION pub_emp;
	DROP SCHEMA sch_emp CASCADE;
	DROP PUBLICATION pub_ins;
	DROP PUBLICATION pub_w;
	DROP PUBLICATION pub_setall;
	DROP PUBLICATION pub_flip;
	DROP PUBLICATION pub_part;
}

session "s1"

step "s1_begin" { BEGIN; }
step "s1_update_nori" { UPDATE sch1.t_nori SET val = 2 WHERE id = 1; }
step "s1_update_w" { UPDATE sch3.t_w SET val = 2 WHERE id = 1; }
step "s1_update_attach" { UPDATE t_attach SET val = 2 WHERE id = 1; }
step "s1_commit" { COMMIT; }

session "s2"

step "s2_add_schema" { ALTER PUBLICATION pub1 ADD TABLES IN SCHEMA sch1; }
step "s2_create_schema" { CREATE PUBLICATION pub_create FOR TABLES IN SCHEMA sch1; }
step "s2_add_empty" { ALTER PUBLICATION pub_emp ADD TABLES IN SCHEMA sch_emp; }
step "s2_add_schema_ins" { ALTER PUBLICATION pub_ins ADD TABLES IN SCHEMA sch2; }
step "s2_set_publish_w" { ALTER PUBLICATION pub_w SET (publish = 'insert, update'); }
step "s2_create_all" { CREATE PUBLICATION pub_all FOR ALL TABLES; }
step "s2_drop_all" { DROP PUBLICATION pub_all; }
step "s2_begin" { BEGIN; }
step "s2_setall" { ALTER PUBLICATION pub_setall SET ALL TABLES; }
step "s2_commit" { COMMIT; }
step "s2_add_schema_flip" { ALTER PUBLICATION pub_flip ADD TABLES IN SCHEMA sch1; }
step "s2_attach" { ALTER TABLE schp.part ATTACH PARTITION t_attach FOR VALUES FROM (1000) TO (2000); }

session "s3"

step "s3_update_ri" { UPDATE sch1.t_ri SET val = 2 WHERE id = 1; }
step "s3_update_nori" { UPDATE sch1.t_nori SET val = 3 WHERE id = 1; }
step "s3_update_ins" { UPDATE sch2.t_ins SET val = 2 WHERE id = 1; }
step "s3_update_w" { UPDATE sch3.t_w SET val = 3 WHERE id = 1; }
step "s3_create_new" { CREATE TABLE t_new (id int, val int); }
step "s3_create_in_emp" { CREATE TABLE sch_emp.t_new (id int, val int); }
step "s3_begin" { BEGIN; }
step "s3_update_new" { UPDATE t_new SET val = 1 WHERE id = 1; }
step "s3_update_emp" { UPDATE sch_emp.t_new SET val = 1 WHERE id = 1; }
step "s3_commit" { COMMIT; }
step "s3_flip" { ALTER TABLE sch1.t_ri REPLICA IDENTITY NOTHING; }
step "s3_update_ri2" { UPDATE sch1.t_ri SET val = 3 WHERE id = 1; }
step "s3_update_attach" { UPDATE t_attach SET val = 3 WHERE id = 1; }

# ALTER PUBLICATION ... ADD TABLES IN SCHEMA waits for an in-progress writer
# on a table in the schema without a replica identity, but a writer on a
# table with a replica identity is not blocked.  Afterwards, writers on the
# table are correctly rejected.
permutation "s1_begin" "s1_update_nori" "s2_add_schema" "s3_update_ri" "s1_commit" "s3_update_nori"

# An insert-only publication does not need a replica identity on its tables,
# so adding a schema to it does not block writers.
permutation "s1_begin" "s1_update_nori" "s2_add_schema_ins" "s3_update_ins" "s1_commit"

# ALTER PUBLICATION ... SET (publish = ...) enabling UPDATE on a TABLES IN
# SCHEMA publication waits for an in-progress writer on a schema member
# without a replica identity.
permutation "s1_begin" "s1_update_w" "s2_set_publish_w" "s1_commit" "s3_update_w"

# CREATE PUBLICATION ... FOR ALL TABLES waits for an in-progress writer on a
# table without a replica identity.
permutation "s1_begin" "s1_update_nori" "s2_create_all" "s1_commit" "s3_update_nori" "s2_drop_all"

# CREATE PUBLICATION ... FOR TABLES IN SCHEMA also waits for an in-progress
# writer on a table in the schema without a replica identity.
permutation "s1_begin" "s1_update_nori" "s2_create_schema" "s1_commit" "s3_update_nori"

# Adding an empty schema still takes the publication catalog lock, so a table
# created in it while the DDL is in progress is covered: a writer on it waits
# for the DDL's catalog lock, then is correctly rejected.
permutation "s2_begin" "s2_add_empty" "s3_create_in_emp" "s3_begin" "s3_update_emp" "s2_commit" "s3_commit"

# ALTER PUBLICATION ... SET ALL TABLES waits for an in-progress writer; a
# table created after it surveyed the existing tables is covered by the
# writer building its publication descriptor taking a conflicting lock on the
# publication catalog, which the DDL holds until commit.
permutation "s1_begin" "s1_update_nori" "s2_begin" "s2_setall" "s1_commit" "s3_create_new" "s3_begin" "s3_update_new" "s2_commit" "s3_commit"

# A table whose replica identity is removed after the DDL surveyed it: a
# writer on it builds its publication descriptor, and waits for the DDL's
# lock on the publication catalog.  Once the DDL has committed, the writer is
# correctly rejected.
permutation "s2_begin" "s2_add_schema_flip" "s3_flip" "s3_begin" "s3_update_ri2" "s2_commit" "s3_commit"

# ATTACH PARTITION takes AccessExclusiveLock on the new partition, so it waits
# for an in-progress writer on it; afterwards the partition is covered by the
# parent's publication and writers are correctly rejected.
permutation "s1_begin" "s1_update_attach" "s2_attach" "s1_commit" "s3_update_attach"

