CREATE EXTENSION test_pg_mkdir_p;

select * from test_pg_mkdir_p(1);

select * from test_pg_mkdir_p(2);
select * from test_pg_mkdir_p(2);

select * from test_pg_mkdir_p(4);
select * from test_pg_mkdir_p(4);

select * from test_pg_mkdir_p(8);
select * from test_pg_mkdir_p(8);

select * from test_pg_mkdir_p(16);
select * from test_pg_mkdir_p(16);

select * from test_pg_mkdir_p(32);
select * from test_pg_mkdir_p(32);
