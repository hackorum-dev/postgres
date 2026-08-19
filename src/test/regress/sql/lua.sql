\if :{?LUA_RELEASE}
  \echo "lua is supported"
\else
  \echo "lua is not supported"
  \quit
\endif

-- lua commands
\luacode
function foo(n, m)
  return n + 1, m
end;
\.

\luacode
print(foo(10, "ahoj"))
\.

\lua foo 10 ahoj
\luaset myvar foo 10 ahoj
\echo :myvar
\unset myvar

create table footable(a int, b int);
insert into footable values(10, 20);
insert into footable values(30, 40);

\luacode
  local con = psql.connect();
  psql.printQuery(con:exec("select * from footable"));
  psql.printQuery(psql.exec("select * from footable"));
\.

\set tablename footable
\luastr psql.printQuery(psql.exec("select * from " .. :"tablename"))

drop table footable;

\luacode
function foo(a, b, c)
  return a, b, c;
end
\.

\luaset a,b,c foo 10 20 30
\echo :a :b :c
\luaset a , b , c foo 10 20 30
\echo :a :b :c

\luaset a ,b , c foo 10 20 30
\echo :a :b :c

\luaset a,b , c foo 10 20 30
\echo :a :b :c

\luaset a ,b, c foo 10 20 30
\echo :a :b :c

