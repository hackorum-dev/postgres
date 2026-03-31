create function ret_settxt() returns setof text as
$$
begin
    return query execute 'select 1 union all select 1';
end;
$$
language plpgsql immutable;

-- check CAST FORMAT expression, the following should all fail
select cast(NULL as date format ret_settxt()); -- cannot return a set
select cast(NULL as date format (select 1::text where false));
select cast(NULL as date format (string_agg(NULL, ' ')));
select cast(NULL as date format (string_agg(NULL, ' ') over () ));
select cast(NULL as date format NULL::int);
select cast('1' as date format B'01');

-- CAST FORMAT is restricted to the source and target types used by to_char, to_date, to_timestamp, and to_number
-- The following should all fail
select cast('hello' as name format 'test');
select cast('hello' as bpchar format 'test');
select cast('-34,338,492' as bigint format '99G999G999');
select cast(array[1] as text format 'YYYY');
select cast('1' as timestamp[] format 'YYYY-MM-DD');
select cast('2012-13-12' as timestamp format 'YYYY-MM-DD');
select cast('2012-13-12' as time format 'YYYY-MM-DD');
select cast('2012-13-12' as timetz format 'YYYY-MM-DD');
select cast('2012-13-12' as interval format 'YYYY-MM-DD');
select cast('1'::text as unknown format 'YYYY-MM-DD');
select cast('1' as bool format 'YYYY-MM-DD');
select cast('1' as json format 'YYYY-MM-DD');
select cast('1'::json as text format 'YYYY-MM-DD');
select cast('1' as anyelement format 'YYYY-MM-DD');
select cast('1' as anyenum format 'YYYY-MM-DD');
select cast('1' as anyarray format 'YYYY-MM-DD');
select cast(null::anyelement as anyelement format 'YYYY-MM-DD');
select cast('2012-12-12 12:00'::timetz as text format 'YYYY-MM-DD HH:MI:SS TZ');
select cast(null::regclass as text format 'YYYY-MM-DD HH:MI:SS TZ');
select cast(null::int2 as numeric format null);
select cast(null::date as timestamptz format null);
select cast(null::time as timestamptz format null);

-- CAST FORMAT is not supported for binary coercible type cast
select cast('2022-01-01' as unknown format null);
select cast('1' as text format '1'::text);
select cast('1'::text as text format '1'::text);

select cast('2012-12-12 12:00'::timetz as text format 'YYYY-MM-DD HH:MI:SS TZ'); -- error
select cast('2012-13-12' as date format 'YYYY-DD-MM'); -- ok
select cast('2012-13-12' as date format 'YYYY-MM-DD'); -- error
select cast('1' as date format 'YYYY-MM-DD');
select cast('1' collate "C" as date format 'YYYY-MM-DD');
select cast('2012-13-12' as date format 'YYYY-DD-MM') as date;
select cast('2012-13-12' as timestamptz format 'YYYY-DD-MM') as date;
select cast('2012-13-12'::text as timestamp format 'YYYY-DD-MM') as date; -- error
select cast('1' as timestamp format 'YYYY-MM-DD') = to_timestamp('1', 'YYYY-MM-DD');
select cast('2026-01-28 13:29:12.324606+01'::text as timestamp format 'YYYY-MM-DD') =
            to_timestamp('2026-01-28 13:29:12.324606+01'::text, 'YYYY-MM-DD');
select cast('1' as date format 'YYYY-MM-DD') = to_date('1', 'YYYY-MM-DD');

-- test with domain
create domain d1 as date check (value <> '0001-01-01');
select cast('1' as text format 'YYYY-MM-DD'); -- error
select cast('1' as d1 format 'YYYY-MM-DD'); -- error
select cast('1' as date format 'MM-DD'); -- ok
select cast('1' as d1 format 'MM-DD'); -- ok

select cast('1'::text collate "C" as date format 'YYYY-MM-DD');
select cast('1' as date format 'YYYY-MM-DD') = to_date('1', 'YYYY-MM-DD') as expect_true;

create table tcast(col1 text, col2 text, col3 date, col4 timestamptz, col5 int8);
insert into tcast(col1, col2, col5)
  values('2022-12-13', 'YYYY-MM-DD', 1234),
        ('2022-12-01', 'YYYY-DD-MM', -1234);

select cast(col1 as date format col2) from tcast;
select cast(col1 as date format col3) from tcast; -- error
select cast(col1 as date format col3::text) from tcast; -- ok

create function imm_const() returns text as $$ begin return 'YYYY-MM-DD'; end; $$ language plpgsql immutable;
select cast(col1 as date format imm_const()) from tcast;
create index s1 on tcast(to_date(col1, 'YYYY-MM-DD')); -- error
create index s1 on tcast(cast(col1 as date format 'YYYY-MM-DD')); -- error

create view tcast_v1 as
   select cast(col1 as date format 'YYYY-MM-DD') as to_date,
         cast(col1 as timestamptz format 'YYYY-MM-DD') as to_timestamptz,
         cast(NULL::interval as text format 'YYYY-MM-DD') as to_txt0,
         cast(col1::timestamp as text format 'YYYY-MM-DD') as to_txt1,
         cast(col3 as text format 'YYYY-MM-DD') as to_txt2,
         cast(col4 as text format 'YYYY-MM-DD') as to_txt3,
         cast(numeric 'inf' as text format 'YYYY-MM-DD') as to_txt4,
         cast(bigint '12324' as text format 'YYYY-MM-DD') as to_txt5
   from tcast;

select pg_get_viewdef('tcast_v1', true);

explain (verbose, costs off)
select cast(col5::float8 as text format '9.99EEEE') as to_txt1,
 cast(col5::numeric as text format '9.99EEEE') as to_txt2
from tcast;

create view tcast_v2 as
   select cast(col5 as text format '9.99EEEE') as to_txt0,
         cast(col5::float8 as text format '9.99EEEE') as to_txt1,
         cast(col5::float4 as text format '9.99EEEE') as to_txt2,
         cast(col5::numeric as text format '9.99EEEE') as to_txt3,
         cast(col5::int2 as text format '9.99EEEE') as to_txt4,
         cast(col5::int4 as text format '9.99EEEE') as to_txt5
   from tcast;
select pg_get_viewdef('tcast_v2', true);
select * from tcast_v2;
drop function ret_settxt;
drop view tcast_v1, tcast_v2;
drop table tcast;
