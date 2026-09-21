# Test for relation level predicate locking in TID range scans
#
# A TID range scan reads a range of heap blocks directly, with no index
# involved, so like a sequential scan it has nothing finer to lock than the
# whole relation.  Verify that the relation level SIREAD lock is acquired, by
# checking that write skew and phantom rows seen through a TID range scan are
# detected.

setup
{
 create table tidrange_tbl (id int, p int);
 insert into tidrange_tbl values (1, 0), (2, 0);
}

teardown
{
 drop table tidrange_tbl;
}

session s1
setup
{
 begin isolation level serializable;
 set enable_seqscan = off;
}
step rxy1	{ select sum(p) from tidrange_tbl
			  where ctid >= '(0,0)' and ctid < '(1,0)'; }
step wx1	{ update tidrange_tbl set p = 1 where ctid = '(0,1)'; }
step wi1	{ insert into tidrange_tbl values (3, 10); }
step c1		{ commit; }

session s2
setup
{
 begin isolation level serializable;
 set enable_seqscan = off;
}
step rxy2	{ select sum(p) from tidrange_tbl
			  where ctid >= '(0,0)' and ctid < '(1,0)'; }
step wy2	{ update tidrange_tbl set p = 1 where ctid = '(0,2)'; }
step wi2	{ insert into tidrange_tbl values (4, 20); }
step c2		{ commit; }

# Both transactions read the whole TID range, then each updates a row that the
# other one read.  No serial order produces sum(p) = 2, so one of them has to
# be aborted.

permutation rxy1 rxy2 wx1 wy2 c1 c2
permutation rxy1 rxy2 wy2 wx1 c1 c2
permutation rxy2 rxy1 wx1 wy2 c2 c1

# Both transactions read the whole TID range, then each inserts a row that
# falls inside the range the other one read.

permutation rxy1 rxy2 wi1 wi2 c1 c2
permutation rxy2 rxy1 wi2 wi1 c2 c1
