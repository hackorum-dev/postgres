begin;

create schema test_distinct;
set search_path to test_distinct;

-- Single relation.
create table a (
  i integer primary key,
  j integer
);

explain (verbose on, costs off)
select distinct i from a;

-- Explicit unique-ification is needed if there's no unique index on the
-- "j" column.
explain (verbose on, costs off)
select distinct j from a;

create unique index on a(j);

-- The unique index alone is not sufficient if the column is nullable.
explain (verbose on, costs off)
select distinct j from a;

alter table a alter column j set not null;

-- Now we only need sequential scan.
explain (verbose on, costs off)
select distinct j from a;

-- Joins.
create table b (
  k integer,
  l integer
);

explain (verbose on, costs off)
select distinct i, l from a join b on i = k;

-- "Inner Unique" is essential to prove that the join output is distinct,
-- but to set it the planner needs an unique index on the inner table.
create unique index on b(k);

-- No explicit unique-ification is needed now.
explain (verbose on, costs off)
select distinct i, l from a join b on i = k;

-- Non-strict join clause prevents the planner from setting "Inner Unique",
-- so explicit unique-ification cannot be skipped.
explain (verbose on, costs off)
select distinct i, l from a join b on i = coalesce(k, 0);

rollback;