--
-- Miscellaneous topics
--

-- Verify that we can parse new-style CREATE FUNCTION/PROCEDURE
do
$$
  declare procedure int;  -- check we still recognize non-keywords as vars
  begin
  create function test1() returns int
    begin atomic
      select 2 + 2;
    end;
  create or replace procedure test2(x int)
    begin atomic
      select x + 2;
    end;
  end
$$;

\sf test1
\sf test2

-- Test %TYPE and %ROWTYPE error cases
create table misc_table(f1 int);

do $$ declare x foo%type; begin end $$;
do $$ declare x notice%type; begin end $$;  -- covers unreserved-keyword case
do $$ declare x foo.bar%type; begin end $$;
do $$ declare x foo.bar.baz%type; begin end $$;
do $$ declare x public.foo.bar%type; begin end $$;
do $$ declare x public.misc_table.zed%type; begin end $$;

do $$ declare x foo%rowtype; begin end $$;
do $$ declare x notice%rowtype; begin end $$;  -- covers unreserved-keyword case
do $$ declare x foo.bar%rowtype; begin end $$;
do $$ declare x foo.bar.baz%rowtype; begin end $$;
do $$ declare x public.foo%rowtype; begin end $$;
do $$ declare x public.misc_table%rowtype; begin end $$;

-- Test handling of an unreserved keyword as a variable name
-- and record field name.
do $$
declare
  execute int;
  r record;
begin
  execute := 10;
  raise notice 'execute = %', execute;
  select 1 as strict into r;
  raise notice 'r.strict = %', r.strict;
end $$;

-- Test handling of a reserved keyword as a record field name.

do $$ declare r record;
begin
  select 1 as x, 2 as foreach into r;
  raise notice 'r.x = %', r.x;
  raise notice 'r.foreach = %', r.foreach;  -- fails
end $$;

do $$ declare r record;
begin
  select 1 as x, 2 as foreach into r;
  raise notice 'r.x = %', r.x;
  raise notice 'r."foreach" = %', r."foreach";  -- ok
end $$;

-- unterminated constructs should complain about the missing terminator
do $$ begin if true $$;
do $$ declare x int; begin x := 1 $$;

--
-- Line numbers of statements whose location the grammar resolves only after
-- their bodies (IF, ELSIF and CASE), so locations reach the scanner out of
-- order
--
create function lineno_order(k int) returns int language plpgsql as $$
declare
  r int;
begin
  if k = 1 then
    r := 1;
  elsif 1 / (k - 2) = 0 then
    r := 2;
  end if;
  case 6 / (k - 3)
    when 0 then
      r := 3;
    else
      if k = 5 then
        r := 1 / (k - 5);
      end if;
      r := 4;
  end case;
  if 1 / (k - 4) = 0 then
    r := 5;
  end if;
  return r;
end
$$;
select lineno_order(2);
select lineno_order(3);
select lineno_order(4);
select lineno_order(5);
drop function lineno_order(int);
