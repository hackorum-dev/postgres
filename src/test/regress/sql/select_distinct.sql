--
-- SELECT_DISTINCT
--

--
-- awk '{print $3;}' onek.data | sort -n | uniq
--
SELECT DISTINCT two FROM tmp ORDER BY 1;

--
-- awk '{print $5;}' onek.data | sort -n | uniq
--
SELECT DISTINCT ten FROM tmp ORDER BY 1;

--
-- awk '{print $16;}' onek.data | sort -d | uniq
--
SELECT DISTINCT string4 FROM tmp ORDER BY 1;

--
-- awk '{print $3,$16,$5;}' onek.data | sort -d | uniq |
-- sort +0n -1 +1d -2 +2n -3
--
SELECT DISTINCT two, string4, ten
   FROM tmp
   ORDER BY two using <, string4 using <, ten using <;

--
-- awk '{print $2;}' person.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - emp.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=1){print $5;}else{print;}}' - stud_emp.data |
-- sort -n -r | uniq
--
SELECT DISTINCT p.age FROM person* p ORDER BY age using >;

--
-- Check mentioning same column more than once
--

EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM
  (SELECT DISTINCT two, four, two FROM tenk1) ss;

SELECT count(*) FROM
  (SELECT DISTINCT two, four, two FROM tenk1) ss;

--
-- Also, some tests of IS DISTINCT FROM, which doesn't quite deserve its
-- very own regression file.
--

CREATE TEMP TABLE disttable (f1 integer);
INSERT INTO DISTTABLE VALUES(1);
INSERT INTO DISTTABLE VALUES(2);
INSERT INTO DISTTABLE VALUES(3);
INSERT INTO DISTTABLE VALUES(NULL);

-- basic cases
SELECT f1, f1 IS DISTINCT FROM 2 as "not 2" FROM disttable;
SELECT f1, f1 IS DISTINCT FROM NULL as "not null" FROM disttable;
SELECT f1, f1 IS DISTINCT FROM f1 as "false" FROM disttable;
SELECT f1, f1 IS DISTINCT FROM f1+1 as "not null" FROM disttable;

-- check that optimizer constant-folds it properly
SELECT 1 IS DISTINCT FROM 2 as "yes";
SELECT 2 IS DISTINCT FROM 2 as "no";
SELECT 2 IS DISTINCT FROM null as "yes";
SELECT null IS DISTINCT FROM null as "no";

-- negated form
SELECT 1 IS NOT DISTINCT FROM 2 as "no";
SELECT 2 IS NOT DISTINCT FROM 2 as "yes";
SELECT 2 IS NOT DISTINCT FROM null as "no";
SELECT null IS NOT DISTINCT FROM null as "yes";
create table select_distinct_a(pk1 int, pk2 char(20),  uk1 char(20) not null,  uk2 int, e int, primary key(pk1, pk2));
create unique index select_distinct_a_uk on select_distinct_a(uk1, uk2);
create table select_distinct_b(a int, b char(20), pk1 char(20), pk2 int, e int, primary key(pk1, pk2));

-- distinct erased since (pk1, pk2)
explain (costs off) select distinct * from select_distinct_a;

-- distinct can't be reased since since we required all the uk must be not null
explain (costs off) select distinct uk1, uk2 from select_distinct_a;

-- distinct ereased since uk + not null
explain (costs off) select distinct uk1, uk2 from select_distinct_a where uk2 is not null;
explain (costs off) select distinct uk1, uk2 from select_distinct_a where uk2 > 1;

-- distinct erased due to group by
explain (costs off) select distinct e from select_distinct_a group by e;

-- distinct erased due to the restirctinfo
explain (costs off) select distinct uk1 from select_distinct_a where pk1 = 1 and pk2 = 'c';

-- test join
set enable_mergejoin to off;
set enable_hashjoin to off;

insert into select_distinct_a values(1, 'a', 'a', 0, 1), (1, 'b', 'A', 0, 2), (3, 'c', 'c', 0, 3);
insert into select_distinct_b values(1, 'a', 'a', 0, 1), (4, 'd', 'd', 0, 4), (1, 'e', 'e', 0, 5);

-- Cartesian join
explain (costs off) select distinct a.uk1, a.uk2, b.pk1, b.pk2 from select_distinct_a a, select_distinct_b b where a.uk2 is not null;
select distinct a.uk1, a.uk2, b.pk1, b.pk2 from select_distinct_a a, select_distinct_b b where a.uk2 is not null order by 1, 2, 3, 4;


-- left join
explain (costs off) select distinct a.pk1, a.pk2, b.pk1, b.pk2 from select_distinct_a a left join select_distinct_b b on (a.pk1 = b.a);
select distinct a.pk1, a.pk2, b.pk1, b.pk2 from select_distinct_a a left join select_distinct_b b on (a.pk1 = b.a) order by 1, 2, 3, 4;;

-- right join
explain (costs off) select distinct a.pk1, a.pk2, b.pk1, b.pk2 from select_distinct_a a right join select_distinct_b b on (a.pk1 = b.a);
select distinct a.pk1, a.pk2, b.pk1, b.pk2 from select_distinct_a a right join select_distinct_b b on (a.pk1 = b.a) order by 1, 2, 3, 4;

-- full join
explain (costs off)  select distinct a.pk1, a.pk2, b.pk1, b.pk2 from select_distinct_a a full outer join select_distinct_b b on (a.pk1 = b.a);
select distinct a.pk1, a.pk2, b.pk1, b.pk2 from select_distinct_a a full outer join select_distinct_b b on (a.pk1 = b.a) order by 1, 2, 3, 4;

-- distinct can't be erased since b.pk2 is missed
explain (costs off) select distinct a.pk1, a.pk2, b.pk1 from select_distinct_a a full outer join select_distinct_b b on (a.pk1 = b.a);


-- Semi/anti join
explain (costs off) select distinct pk1, pk2 from select_distinct_a where pk1 in (select a from select_distinct_b);
explain (costs off) select distinct pk1, pk2 from select_distinct_a where pk1 not in (select a from select_distinct_b);

-- we also can handle some limited subquery
explain (costs off) select distinct * from select_distinct_a a,  (select a from select_distinct_b group by a) b where a.pk1 = b.a;
select distinct * from select_distinct_a a,  (select a from select_distinct_b group by a) b where a.pk1 = b.a order by 1, 2, 3;

explain (costs off) select distinct * from select_distinct_a a,  (select distinct a from select_distinct_b) b where a.pk1 = b.a;
select distinct * from select_distinct_a a, (select distinct a from select_distinct_b) b where a.pk1 = b.a order by 1 ,2, 3;

-- Distinct On
-- can't erase since pk2 is missed
explain (costs off) select distinct on(pk1) pk1, pk2 from select_distinct_a;
-- ok to erase
explain (costs off) select distinct on(pk1, pk2) pk1, pk2 from select_distinct_a;


-- test some view.
create view distinct_v1 as select distinct uk1, uk2 from select_distinct_a where uk2 is not null;
explain (costs off) select * from distinct_v1;

alter table select_distinct_a alter column uk1 drop not null;
explain (costs off) select * from distinct_v1;

alter table select_distinct_a alter column uk1 set not null;

-- test generic plan
prepare pt as select * from distinct_v1;
explain (costs off)  execute pt;
alter table select_distinct_a alter column uk1 drop not null;
explain (costs off) execute pt;

drop view distinct_v1;
drop table select_distinct_a;
drop table select_distinct_b;
