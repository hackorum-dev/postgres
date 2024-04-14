CREATE EXTENSION pageinspect;
CREATE EXTENSION test_heapam;

CREATE TABLE prunetest (data text) WITH (autovacuum_enabled=false);

select pg_current_xact_id() as committed_xid
\gset

begin;
select pg_current_xact_id() as aborted_xid;
\gset
rollback;

create temporary view dump_items as
  SELECT lp, case when lp_flags = 2 then lp_off else null end as redirect_off, t_xmin, t_xmax,
  (heap_tuple_infomask_flags(t_infomask, t_infomask2)).*,
  convert_from(substring(t_data, 2), 'utf8') as data
  FROM heap_page_items(get_raw_page('prunetest', 0));

select heappage_craft_new();
select heappage_craft_add_lp_unused('1');
select heappage_craft_add_tuple('1', :'committed_xid', '0', '0', '(0, 1)', array[]::text[], 'normal');
select heappage_craft_install('prunetest'::regclass, 0);

select * from dump_items;
--SELECT *, (heap_tuple_infomask_flags(t_infomask, t_infomask2)).* FROM heap_page_items(get_raw_page('prunetest', 0));

SELECT heappage_prune_and_freeze('prunetest', 0);

select * from dump_items;


-- Page has two LP_DEAD items, one LP_UNUSED, nothing else.
select heappage_craft_new();
select heappage_craft_add_lp_dead('1');
select heappage_craft_add_lp_unused('2');
select heappage_craft_add_lp_dead('3');
select heappage_craft_install('prunetest'::regclass, 0);

select heappage_prune_and_freeze('prunetest', 0);

-- One aborted item, nothing else. 
select heappage_craft_new();
select heappage_craft_add_tuple('1', :'aborted_xid', '0', '0', '(0, 1)', array[]::text[], 'aborted');
select heappage_craft_install('prunetest'::regclass, 0);
select heappage_prune_and_freeze('prunetest', 0);

-- One already-frozen item, nothing else. 
select heappage_craft_new();
select heappage_craft_add_tuple('1', '2', '0', '0', '(0, 1)', array[]::text[], 'normal');
select heappage_craft_install('prunetest'::regclass, 0);
select heappage_prune_and_freeze('prunetest', 0);

-- One committed item, nothing else.
select pg_current_xact_id() as committed_xid
\gset
select heappage_craft_new();
select heappage_craft_add_tuple('1', :'committed_xid', '0', '0', '(0, 1)', array[]::text[], 'normal');
select heappage_craft_install('prunetest'::regclass, 0);
select heappage_prune_and_freeze('prunetest', 0);


-- One visible item, one deleted item
select pg_current_xact_id() as committed_xid1
\gset

select pg_current_xact_id() as committed_xid2
\gset

select heappage_craft_new();
select heappage_craft_add_tuple('1', :'committed_xid1', '0', '0', '(0, 1)', array[]::text[], 'normal');
select heappage_craft_add_tuple('2', '2', :'committed_xid2', '0', '(0, 1)', array[]::text[], 'deleted');
select heappage_craft_install('prunetest'::regclass, 0);
select * from dump_items;
select heappage_prune_and_freeze('prunetest', 0);
select * from dump_items;
