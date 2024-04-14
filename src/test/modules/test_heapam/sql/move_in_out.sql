CREATE EXTENSION pageinspect;
CREATE EXTENSION test_heapam;

CREATE TEMP TABLE prunetest (data text);

select pg_current_xact_id() as committed_xid1
\gset

create temporary view dump_items as
  SELECT lp, case lp_flags
    when 0 then 'LP_UNUSED'
    when 1 then 'LP_NORMAL'
    when 2 then 'LP_REDIRECT to ' || lp_off
    when 3 then 'LP_DEAD' END as lp_flags,
    t_xmin, t_xmax, t_field3,
  (heap_tuple_infomask_flags(t_infomask, t_infomask2)).*,
  convert_from(substring(t_data, 2), 'utf8') as data
  FROM heap_page_items(get_raw_page('prunetest', 0));

select heappage_craft_new();

--				lp    xmin  xmax   xvac/cid           ctid        flags                              data
select heappage_craft_add_tuple('1',  '2',  '0',   :'committed_xid1', '(0, 1)',   array['HEAP_MOVED_OFF']::text[],   'normal');
select heappage_craft_add_tuple('2',  '2',  '0',   :'committed_xid1', '(0, 2)',   array['HEAP_MOVED_IN']::text[],    'normal');

select heappage_craft_install('prunetest'::regclass, 0);

select * from dump_items;

select xmin, xmax, ctid, * from prunetest;

select * from dump_items;
--SELECT *, (heap_tuple_infomask_flags(t_infomask, t_infomask2)).* FROM heap_page_items(get_raw_page('prunetest', 0));

vacuum freeze prunetest;

select * from dump_items;
