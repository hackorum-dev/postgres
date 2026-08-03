--
-- PARALLEL INSERT ... SELECT
--

--
-- START: setup some functions needed by the tests.
--

-- For testing purposes, we'll mark these functions as parallel-unsafe or
-- parallel-restricted; plpgsql so that they are not inlined into the
-- stored expressions
create function fullname_parallel_unsafe(f text, l text) returns text
language plpgsql immutable parallel unsafe as $$
begin
	return f || l;
end; $$;

create function fullname_parallel_restricted(f text, l text) returns text
language plpgsql immutable parallel restricted as $$
begin
	return f || l;
end; $$;

create function bdefault_unsafe() returns int
language plpgsql parallel unsafe as $$
begin
	return 5;
end; $$;

create function cdefault_restricted() returns int
language plpgsql parallel restricted as $$
begin
	return 10;
end; $$;

create function trg_unsafe_fn() returns trigger
language plpgsql parallel unsafe as $$
begin
	return new;
end; $$;

create function trg_restricted_fn() returns trigger
language plpgsql parallel restricted as $$
begin
	return new;
end; $$;

--
-- END: setup.
--

begin;

-- encourage use of parallel plans
set parallel_setup_cost = 0;
set parallel_tuple_cost = 0;
set min_parallel_table_scan_size = 0;
set max_parallel_workers_per_gather = 4;

-- force seq scans, for deterministic plans
set enable_indexonlyscan = off;
set enable_indexscan = off;
set enable_bitmapscan = off;

--
-- A plain target table has no parallel-unsafe objects, so the SELECT part
-- can be parallelized.
--
create table para_insert (a int, b name);

explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;
insert into para_insert select unique1, stringu1 from tenk1;

-- select some values to verify that the parallel insert worked
select count(*), sum(a) from para_insert;
-- verify that the same transaction and command has been used for all rows
select count(*) from (select distinct cmin, xmin from para_insert) as dt;

-- INSERT ... VALUES (no underlying SELECT) can't be parallelized
explain (costs off) insert into para_insert values (1, 'one');

-- a parallel-unsafe function in the SELECT part can't be parallelized
explain (costs off)
insert into para_insert select unique1, fullname_parallel_unsafe(stringu1, 'x') from tenk1;

--
-- A parallel-unsafe trigger on the target table prevents parallel SELECT.
--
create trigger trg before insert on para_insert
	for each row execute function trg_unsafe_fn();
explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;
-- dropping it makes the target parallel-safe again
drop trigger trg on para_insert;
explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;

--
-- A parallel-restricted trigger on the target table still allows parallel
-- SELECT; the trigger fires in the leader.
--
create trigger trg before insert on para_insert
	for each row execute function trg_restricted_fn();
explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;
drop trigger trg on para_insert;

--
-- A parallel-unsafe check constraint on the target table prevents
-- parallel SELECT.
--
alter table para_insert add constraint chk_u check (fullname_parallel_unsafe(b, 'x') <> '');
explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;
alter table para_insert drop constraint chk_u;
explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;

--
-- A parallel-unsafe index expression on the target table prevents
-- parallel SELECT.
--
create index para_insert_unsafe_idx on para_insert (fullname_parallel_unsafe(b, 'x'));
explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;
drop index para_insert_unsafe_idx;
explain (costs off) insert into para_insert select unique1, stringu1 from tenk1;

--
-- Column defaults are checked: an unsafe default prevents parallel SELECT,
-- while a restricted one is allowed (defaults are evaluated in the leader).
--
create table testdef (a int,
					  b int default bdefault_unsafe(),
					  c int default cdefault_restricted());
explain (costs off) insert into testdef select unique1 from tenk1;
alter table testdef alter column b drop default;
explain (costs off) insert into testdef select unique1 from tenk1;

--
-- For an INSERT into a partitioned table, every partition must be
-- parallel-safe.
--
create table part_para_insert (a int) partition by range (a);
create table part_para_insert_p1 partition of part_para_insert
	for values from (0) to (5000);
create table part_para_insert_p2 partition of part_para_insert
	for values from (5000) to (10000);

explain (costs off) insert into part_para_insert select unique1 from tenk1;
insert into part_para_insert select unique1 from tenk1;
select count(*) from part_para_insert;

-- inserting into a partition directly is fine too
explain (costs off) insert into part_para_insert_p1 select unique1 from tenk1;

-- a pre-existing unsafe trigger on one partition prevents parallel SELECT
-- into the parent
create table part_para_unsafe (a int) partition by range (a);
create table part_para_unsafe_p1 partition of part_para_unsafe
	for values from (0) to (10000);
create trigger trg before insert on part_para_unsafe_p1
	for each row execute function trg_unsafe_fn();
explain (costs off) insert into part_para_unsafe select unique1 from tenk1;

--
-- Changing a function's parallel safety invalidates the cached safety of
-- the target table.
--
create function alterable_fn(int) returns bool
language plpgsql immutable parallel safe as $$
begin
	return $1 > 0;
end; $$;

create table fn_para_insert (a int check (alterable_fn(a)));
explain (costs off) insert into fn_para_insert select unique1 from tenk1;
alter function alterable_fn(int) parallel unsafe;
explain (costs off) insert into fn_para_insert select unique1 from tenk1;
alter function alterable_fn(int) parallel safe;
explain (costs off) insert into fn_para_insert select unique1 from tenk1;

--
-- INSERT ... ON CONFLICT DO UPDATE can't be parallelized, because UPDATE is
-- not supported in parallel mode; ON CONFLICT DO NOTHING can.
--
create table para_insert_pk (a int primary key, b name);
explain (costs off)
insert into para_insert_pk select unique1, stringu1 from tenk1
	on conflict (a) do update set b = excluded.b;
explain (costs off)
insert into para_insert_pk select unique1, stringu1 from tenk1
	on conflict (a) do nothing;

--
-- A temporary or foreign target table is parallel-restricted: only the
-- leader can modify it, but the SELECT part can still be parallelized.
--
create temp table temp_para_insert (a int);
explain (costs off) insert into temp_para_insert select unique1 from tenk1;

--
-- Forcing parallel mode when the plan has no Gather must work too; the
-- transaction ID has to be assigned before entering parallel mode.
--
set debug_parallel_query = on;
explain (costs off) insert into para_insert select i, 'x'::name from generate_series(1, 10) i;
insert into para_insert select i, 'x'::name from generate_series(1, 10) i;
select count(*) from para_insert;
set debug_parallel_query = off;

rollback;

--
-- Cached-plan invalidation when the parallel safety of the target table
-- changes.  (This is outside the transaction block above, so that plan
-- cache effects of previous statements don't leak in.)
--
-- A plan whose shape depended on the target table's parallel DML safety
-- is marked with dependsOnParallelDmlSafety, and is invalidated whenever
-- the parallel DML safety invalidation message arrives for its target
-- (including messages propagated from partitions) or for all relations
-- (a function's parallel safety changed).
--

-- encourage use of parallel plans (again, the ones above were reset by
-- the rollback)
set parallel_setup_cost = 0;
set parallel_tuple_cost = 0;
set min_parallel_table_scan_size = 0;
set max_parallel_workers_per_gather = 4;

-- force use of cached generic plans
set plan_cache_mode = force_generic_plan;

-- adding an unsafe trigger to the target table invalidates the cached
-- plan (via the relcache invalidation of the table), so the next EXECUTE
-- builds a new, non-parallel plan
create table pi_plan_a (a int, b name);
prepare pa as insert into pi_plan_a select unique1, stringu1 from tenk1;
explain (costs off) execute pa;
create trigger trg before insert on pi_plan_a
	for each row execute function trg_unsafe_fn();
explain (costs off) execute pa;
deallocate pa;
-- adding an unsafe trigger to a partition of the target table invalidates
-- the cached plan: the invalidation message is propagated to the
-- partitioned root, and the plan's modify target matches it; dropping the
-- trigger invalidates it again, and it is rebuilt as a parallel plan
create table pi_plan_b (a int, b name) partition by range (a);
create table pi_plan_b_p1 partition of pi_plan_b for values from (0) to (5000);
create table pi_plan_b_p2 partition of pi_plan_b for values from (5000) to (maxvalue);
prepare pb as insert into pi_plan_b select unique1, stringu1 from tenk1;
explain (costs off) execute pb;
create trigger trg before insert on pi_plan_b_p1
	for each row execute function trg_unsafe_fn();
explain (costs off) execute pb;
drop trigger trg on pi_plan_b_p1;
explain (costs off) execute pb;
deallocate pb;
-- altering the parallel safety of a function used in the plan invalidates
-- the cached plan (via the pg_proc invalidation)
create function alterable_fn(int) returns bool
language plpgsql immutable parallel safe as $$
begin
	return $1 > 0;
end; $$;
create table pi_plan_c (a int, b name);
prepare pc as insert into pi_plan_c select unique1, stringu1 from tenk1
	where alterable_fn(unique1);
explain (costs off) execute pc;
alter function alterable_fn(int) parallel unsafe;
explain (costs off) execute pc;
deallocate pc;

-- altering the parallel safety of a function that is only used by the
-- target table's trigger invalidates the cached plan too: a function
-- parallel-safety change invalidates all plans that consulted a target's
-- hazard (intentionally coarse --- the plan is rebuilt even though this
-- function might be unrelated to the plan's target); altering it back
-- rebuilds the parallel plan
create function trg_alterable_fn() returns trigger
language plpgsql parallel safe as $$
begin
	return new;
end; $$;
create table pi_plan_d (a int, b name);
create trigger trg before insert on pi_plan_d
	for each row execute function trg_alterable_fn();
prepare pd as insert into pi_plan_d select unique1, stringu1 from tenk1;
explain (costs off) execute pd;
alter function trg_alterable_fn() parallel unsafe;
explain (costs off) execute pd;
alter function trg_alterable_fn() parallel safe;
explain (costs off) execute pd;
execute pd;
select count(*) from pi_plan_d;
deallocate pd;
-- the reverse direction also works: a plan built while the query's own
-- function is parallel-unsafe (so the target's hazard was never even
-- consulted) is rebuilt as a parallel plan when the function becomes
-- parallel-safe
create function unsafe_to_safe_fn(int) returns bool
language plpgsql immutable parallel unsafe as $$
begin
	return $1 > 0;
end; $$;
prepare pe as insert into pi_plan_c select unique1, stringu1 from tenk1
	where unsafe_to_safe_fn(unique1);
explain (costs off) execute pe;
alter function unsafe_to_safe_fn(int) parallel safe;
explain (costs off) execute pe;
deallocate pe;

reset plan_cache_mode;

-- clean up objects created outside the transaction block
drop table pi_plan_a, pi_plan_b, pi_plan_c, pi_plan_d;
drop function alterable_fn(int);
drop function trg_alterable_fn();
drop function unsafe_to_safe_fn(int);
drop function fullname_parallel_unsafe(text, text);
drop function fullname_parallel_restricted(text, text);
drop function bdefault_unsafe();
drop function cdefault_restricted();
drop function trg_unsafe_fn();
drop function trg_restricted_fn();
