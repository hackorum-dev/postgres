#
# Test assumes there is already setup so
#
# PG server for publisher (running on port 7651)
# - has TABLE tbl
# - has PUBLICATION pub1
#
# PG server for subscriber (running on port 7652)
# - has TABLE tbl
# - has SUBSCRIPTION sub1 subscribing to pub1
#
# ~~~
#
# Now the following spec tests are configured to have multiple sessions on the
# publisher so we can interleave the WAL records to see the effect for the
# subscription
#
# ~~~
#
# How to run
# 1. Run . ./test_init.sh to create the PG instances, table, and publication/subscription
# 2. Run the isolation tester make check-pub-sub (runs this spec)
# 3. Check the logs of the PG instances
#

# Set the isolationtester controller's conninfo. User sessions will also use
# this unless they specify otherwise.
conninfo "host=localhost port=7651"

################
# Publisher node
################
session ps1
setup
{
	TRUNCATE TABLE tbl;
}
step ps1_ins		{ INSERT INTO tbl VALUES (111); }
step ps1_sel		{ SELECT * FROM tbl ORDER BY id; }
step ps1_begin		{ BEGIN; }
step ps1_commit		{ COMMIT; }
step ps1_rollback	{ ROLLBACK; }

session ps2
step ps2_ins		{ INSERT INTO tbl VALUES (222); }
step ps2_sel		{ SELECT * FROM tbl ORDER BY id; }
step ps2_begin		{ BEGIN; }
step ps2_commit		{ COMMIT; }
step ps2_rollback	{ ROLLBACK; }

#################
# Subscriber node
#################
session sub
conninfo "host=localhost port=7652"
setup
{
	TRUNCATE TABLE tbl;
}
step sub_sleep		{ SELECT pg_sleep(3); }
step sub_sel		{ SELECT * FROM tbl ORDER BY id; }

#######
# Tests
#######

# single tx
permutation ps1_begin ps1_ins ps1_commit ps1_sel ps2_sel sub_sleep sub_sel
permutation ps2_begin ps2_ins ps2_commit ps1_sel ps2_sel sub_sleep sub_sel

# rollback
permutation ps1_begin ps1_ins ps1_rollback ps1_sel sub_sleep sub_sel

# overlapping tx rollback and commit
permutation ps1_begin ps1_ins ps2_begin ps2_ins ps1_rollback ps2_commit sub_sleep sub_sel
permutation ps1_begin ps1_ins ps2_begin ps2_ins ps1_commit ps2_rollback sub_sleep sub_sel

# overlapping tx commits
permutation ps1_begin ps1_ins ps2_begin ps2_ins ps2_commit ps1_commit sub_sleep sub_sel
permutation ps1_begin ps1_ins ps2_begin ps2_ins ps1_commit ps2_commit sub_sleep sub_sel

permutation ps1_begin ps2_begin ps1_ins ps2_ins ps2_commit ps1_commit sub_sleep sub_sel
permutation ps1_begin ps2_begin ps1_ins ps2_ins ps1_commit ps2_commit sub_sleep sub_sel

permutation ps1_begin ps2_begin ps2_ins ps1_ins ps2_commit ps1_commit sub_sleep sub_sel
permutation ps1_begin ps2_begin ps2_ins ps1_ins ps1_commit ps2_commit sub_sleep sub_sel
