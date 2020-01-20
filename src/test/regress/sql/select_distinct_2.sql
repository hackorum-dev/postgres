create table select_distinct_a(a int, b char(20),  c char(20) not null,  d int, e int, primary key(a, b));

set enable_mergejoin to off;
set enable_hashjoin to off;

-- no node for distinct.
explain (costs off) select distinct * from select_distinct_a;

explain (costs off) select distinct b,c,d,e from select_distinct_a;

create unique index select_distinct_a_uk on select_distinct_a(c, d);

explain (costs off) select distinct b,c,d,e from select_distinct_a where c is not null;

explain (costs off) select distinct b,c,d,e from select_distinct_a where c is not null and d is not null;

explain select distinct d, e from select_distinct_a group by d, e;


create table select_distinct_b(a int, b char(20),  c char(20) not null,  d int, e int, primary key(a, b));

explain (costs off) select distinct * from select_distinct_a a, select_distinct_b b;

explain (costs off) select distinct a.b, a.c, b.a, b.b from select_distinct_a a, select_distinct_b b;

explain (costs off) select distinct a.d, a.c, b.a, b.b from select_distinct_a a, select_distinct_b b where a.d is not null;

explain (costs off) select distinct a.d, b.a from select_distinct_a a, select_distinct_b b group by a.d, b.a;

drop table select_distinct_a;
drop table select_distinct_b;
