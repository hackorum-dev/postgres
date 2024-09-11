#
# Accept invalidation messages before the start of a new query inside the
# transaction
#

setup
{
  CREATE FUNCTION test_fn() RETURNS integer LANGUAGE sql AS $$ SELECT 1; $$;
}

session s1
setup { BEGIN; }
step s1_exec_fn { SELECT test_fn(); }
step s1_end { END; }

session s2
step s2_drop { DROP FUNCTION test_fn; }
step s2_replace { CREATE OR REPLACE FUNCTION test_fn() RETURNS integer LANGUAGE sql AS $$ SELECT 2; $$; }

permutation
  s1_exec_fn
  s2_replace
  s1_exec_fn
  s2_drop
  s1_exec_fn
  s1_end