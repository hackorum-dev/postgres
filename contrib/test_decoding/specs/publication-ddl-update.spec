# Test that publication DDL naming a table serializes with a concurrent
# data-modifying statement on it: the DDL waits for the writer, so the
# writer's change is decoded while the table is not yet published and is not
# sent at all.  Without that lock, the DDL could commit while the statement
# is in progress, and the change would be decoded under the new publication
# definition and sent without replica identity data, which the subscriber
# cannot apply.

setup
{
    SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'pgoutput');
    CREATE TABLE tab1 (id int, val int);
    INSERT INTO tab1 VALUES (1, 1);
    CREATE PUBLICATION pub;
    CREATE TABLE tab2 (id int, val int);
    INSERT INTO tab2 VALUES (1, 1);
    CREATE PUBLICATION pub_ins FOR TABLE tab2 WITH (publish = 'insert');
    CREATE TABLE part1 (id int, val int) PARTITION BY RANGE (id);
    CREATE TABLE part1_1 PARTITION OF part1 FOR VALUES FROM (0) TO (1000);
    INSERT INTO part1_1 VALUES (1, 1);
    CREATE TABLE part_exc (id int, val int) PARTITION BY RANGE (id);
    CREATE TABLE part_exc1 PARTITION OF part_exc FOR VALUES FROM (0) TO (1000);
    INSERT INTO part_exc1 VALUES (1, 1);
    CREATE TABLE t_other (id int, val int);
    INSERT INTO t_other VALUES (1, 1);
}

teardown
{
    DROP TABLE tab1;
    DROP TABLE tab2;
    DROP TABLE part1 CASCADE;
    DROP TABLE part_exc CASCADE;
    DROP TABLE t_other;
    DROP PUBLICATION pub;
    DROP PUBLICATION pub_ins;
    DROP PUBLICATION IF EXISTS pub_exc;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s1"
setup { SET synchronous_commit=on; }

step "s1_begin" { BEGIN; }
step "s1_update" { UPDATE tab1 SET val = 2 WHERE id = 1; }
step "s1_commit" { COMMIT; }

session "s2"
setup { SET synchronous_commit=on; }

step "s2_add_table" { ALTER PUBLICATION pub ADD TABLE tab1 WHERE (val = 2 OR val = 1); }
step "s2_get_binary_changes" { SELECT count(data) FROM pg_logical_slot_get_binary_changes('isolation_slot', NULL, NULL, 'proto_version', '4', 'publication_names', 'pub') WHERE get_byte(data, 0) = 85; }

session "s3"
setup { SET synchronous_commit=on; }

step "s3_begin" { BEGIN; }
step "s3_update" { UPDATE tab2 SET val = 2 WHERE id = 1; }
step "s3_commit" { COMMIT; }

session "s4"

step "s4_set_publish" { ALTER PUBLICATION pub_ins SET (publish = 'insert, update'); }
step "s4_check" { UPDATE tab2 SET val = 3 WHERE id = 1; }

session "s5"

step "s5_begin" { BEGIN; }
step "s5_update_leaf" { UPDATE part1_1 SET val = 2 WHERE id = 1; }
step "s5_commit" { COMMIT; }
step "s5_check_leaf" { UPDATE part1_1 SET val = 3 WHERE id = 1; }

session "s6"

step "s6_begin" { BEGIN; }
step "s6_update_exc" { UPDATE part_exc1 SET val = 2 WHERE id = 1; }
step "s6_commit" { COMMIT; }
step "s6_check_exc" { UPDATE part_exc1 SET val = 3 WHERE id = 1; }

session "s7"

step "s7_add_parent" { ALTER PUBLICATION pub ADD TABLE part1; }
step "s7_create_exc" { CREATE PUBLICATION pub_exc FOR ALL TABLES EXCEPT (TABLE part_exc); }
step "s7_unexcept" { ALTER PUBLICATION pub_exc SET ALL TABLES EXCEPT (TABLE t_other); }

# LOGICAL_REP_MSG_UPDATE = 'U' = 85.  ALTER PUBLICATION ... ADD TABLE waits
# for the in-progress UPDATE, so the UPDATE commits before the table is
# published and is not sent.  Without the lock, the DDL would commit first
# and the change would be sent without replica identity data.
permutation "s1_begin" "s1_update" "s2_add_table" "s1_commit" "s2_get_binary_changes"

# ALTER PUBLICATION ... SET (publish = ...) enabling UPDATE waits for an
# in-progress writer on a member table; afterwards, writers on it are
# rejected since it has no replica identity.
permutation "s3_begin" "s3_update" "s4_set_publish" "s3_commit" "s4_check"

# A partitioned table's partitions are implicitly published with it, so
# adding the parent waits for an in-progress writer on a partition without a
# replica identity; afterwards, writers on it are rejected.
permutation "s5_begin" "s5_update_leaf" "s7_add_parent" "s5_commit" "s5_check_leaf"

# Removing an exclusion of a partitioned table from a FOR ALL TABLES
# publication publishes its partitions, so the DDL waits for an in-progress
# writer on a partition without a replica identity; afterwards, writers on
# it are rejected.
permutation "s7_create_exc" "s6_begin" "s6_update_exc" "s7_unexcept" "s6_commit" "s6_check_exc"
