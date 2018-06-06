-- Test pushdown of quals into subqueries.

create table smalltab (i int4, j int4);
create table bigtab (i int4, j int4);

insert into smalltab values (1, 1), (100000, 100000);
insert into bigtab select g,g from generate_series(1, 100000) g;

analyze smalltab, bigtab;

create index bigtab_i on bigtab (i);

-- Push down restriction quals.
explain (costs off)
select * from smalltab,
(
  select bigtab.i, avg(bigtab.j)
  from bigtab
  group by bigtab.i
) as subq(i, avg)
where smalltab.i = subq.i and smalltab.i = 123;

-- Join quals are not currently pushed down
explain (costs off)
select * from smalltab,
(
  select bigtab.i, avg(bigtab.j)
  from bigtab
  group by bigtab.i
) as subq(i, avg)
where smalltab.i = subq.i;

-- Except when the subquery is LATERAL, and already references the other relation.
-- Such join clauses can be pushed down.
explain (costs off)
select * from smalltab,
lateral (
  select bigtab.i, avg(bigtab.j)
  from bigtab
  where bigtab.j = smalltab.j
  group by bigtab.i
) as subq(i, avg)
where smalltab.i < subq.i;

-- Multiple join clauses constructed from equivalence classes
explain (costs off)
select * from smalltab,
lateral (
  select bigtab.i, bigtab.j, avg(bigtab.j)
  from bigtab
  where bigtab.j/2 = smalltab.j / 2
  group by bigtab.i, bigtab.j
) as subq(i, j, avg)
where smalltab.i = subq.i and smalltab.j = subq.j;
