create table t(level_1 text, level_2 text, level_3 text);

insert into t
values
('l11', 'l21', 'l31'),
('l11', 'l21', 'l32'),
('l11', 'l21', 'l33'),
('l11', 'l22', 'l34'),
('l11', 'l22', 'l35'),
('l11', 'l22', 'l36');

create statistics on level_1, level_2, level_3 from t;

analyze t;

explain select * from t t1 join t t2 using(level_1, level_2, level_3);
INFO:  n_distinct = 1.000000
INFO:  n_distinct = 2.000000
INFO:  n_distinct = 6.000000
INFO:  2
INFO:  1
INFO:  0
                                              QUERY PLAN
------------------------------------------------------------------------------------------------------
 Hash Join  (cost=1.17..2.32 rows=6 width=12)
   Hash Cond: ((t1.level_1 = t2.level_1) AND (t1.level_2 = t2.level_2) AND (t1.level_3 = t2.level_3))
   ->  Seq Scan on t t1  (cost=0.00..1.06 rows=6 width=12)
   ->  Hash  (cost=1.06..1.06 rows=6 width=12)
         ->  Seq Scan on t t2  (cost=0.00..1.06 rows=6 width=12)
(5 rows)
