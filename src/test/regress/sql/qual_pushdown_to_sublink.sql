CREATE SCHEMA IF NOT EXISTS test_push_qual_to_sublink;
SET search_path=test_push_qual_to_sublink,sys;

show lazy_process_sublink;

create table ab (a int not null, b int not null) partition by list (a);
create table ab_a2 partition of ab for values in(2) partition by list (b);
create table ab_a2_b1 partition of ab_a2 for values in (1);
create table ab_a2_b2 partition of ab_a2 for values in (2);
create table ab_a2_b3 partition of ab_a2 for values in (3);
create table ab_a1 partition of ab for values in(1) partition by list (b);
create table ab_a1_b1 partition of ab_a1 for values in (1);
create table ab_a1_b2 partition of ab_a1 for values in (2);
create table ab_a1_b3 partition of ab_a1 for values in (3);

INSERT INTO ab VALUES (1,1);
INSERT INTO ab VALUES (1,2);
INSERT INTO ab VALUES (1,3);
INSERT INTO ab VALUES (2,1);
INSERT INTO ab VALUES (2,2);
INSERT INTO ab VALUES (2,3);

--1 sublink in select clause can do pushdown qual(a=1 and b=1)
EXPLAIN (VERBOSE, COSTS OFF) SELECT 
y.a, (SELECT x.b FROM ab x WHERE y.a=x.a AND y.b=x.b) AS b
FROM ab y WHERE a = 1 AND b = 1;

SELECT
y.a, (SELECT x.b FROM ab x WHERE y.a=x.a AND y.b=x.b) AS b
FROM ab y WHERE a = 1 AND b = 1;

--2 sublink in where clause can do pushdown qual(a=1 and b=1)
EXPLAIN (VERBOSE, COSTS OFF) SELECT y.a FROM ab y
WHERE a = 1 AND b = 1 AND a in (SELECT x.b FROM ab x WHERE y.a=x.a AND y.b=x.b);

SELECT y.a FROM ab y
WHERE a = 1 AND b = 1 AND a in (SELECT x.b FROM ab x WHERE y.a=x.a AND y.b=x.b);

--3 Nested sublink also supports pushdown qual 
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a.a, a.b,
(SELECT max(b.a) AS max
      FROM ab b, ab c
      WHERE b.a=c.a AND b.b=c.b AND b.a=a.a AND b.b=a.b AND
	(EXISTS 
	 (SELECT a1.a
	  FROM ab a1
	  WHERE
	  a1.a = a.a AND --from uplevel 1
	  a1.b = b.b AND --frem uplevel 2
	  clock_timestamp() > '2020-12-11' --Keep sublink not eliminated
	 )
	)
) AS c
FROM ab a WHERE a.a=1 AND a.b=1;

--4 This feature does not conflict with pullUp sublink
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a.a, a.b
FROM ab a
WHERE EXISTS (SELECT b.a
      FROM ab b, ab c
      WHERE b.a=c.a AND b.b=c.b AND b.a=a.a AND b.b=a.b AND
	(EXISTS
	 (SELECT a1.a
	  FROM ab a1
	  WHERE a1.a = b.a AND a1.b = b.b
	 )
	)
) AND
a.a=1 AND a.b=1;

--5 aggrefs with multiple agglevelsup
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 
	(SELECT 
	 (SELECT sum(foo.a + bar.b) FROM ab jazz WHERE jazz.a=foo.a AND jazz.b=foo.b)
	 FROM ab bar WHERE bar.a=foo.a AND bar.b=foo.b
	) FROM ab foo WHERE foo.a=1 AND foo.b=1 GROUP BY a, b;

--6 sublink in join on clause can not do pushdown
EXPLAIN (VERBOSE, COSTS OFF) SELECT y.a
FROM ab y JOIN ab z on ((y.a=z.a) AND (y.b=z.b) AND exists (SELECT count(*) FROM ab x WHERE x.a=y.a AND x.b=y.b))
WHERE y.a = 1 AND y.b = 1;

DROP SCHEMA test_push_qual_to_sublink CASCADE;
