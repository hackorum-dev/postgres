# Tests for schema drop with concurrently-created objects
#
# When an empty namespace is being initially populated with the below
# objects, it is possible to DROP SCHEMA without a CASCADE before the
# objects are committed.  DROP SCHEMA should wait for the transaction
# creating the given objects to commit before being able to perform the
# schema deletion, and should drop all objects associated with it.

setup
{
	CREATE SCHEMA testschema;
}

session "s1"
step "s1_begin"           { BEGIN; }
step "s1_create_type"     { CREATE TYPE testschema.testtype; }
step "s1_create_agg"      { CREATE AGGREGATE testschema.a1(int4) (SFUNC=int4pl, STYPE=int4); }
step "s1_create_func"     { CREATE FUNCTION testschema.f1() RETURNS bool LANGUAGE SQL AS 'SELECT true'; }
step "s1_create_op"       { CREATE OPERATOR testschema.@+@ (LEFTARG=int4, RIGHTARG=int4, PROCEDURE=int4pl);}
step "s1_create_opfamily" { CREATE OPERATOR FAMILY testschema.opfam1 USING btree; }
step "s1_create_opclass"  { CREATE OPERATOR CLASS testschema.opclass1 FOR TYPE uuid USING hash AS STORAGE uuid; }
step "s1_commit"          { COMMIT; }

session "s2"
step "s2_drop_schema"     { DROP SCHEMA testschema CASCADE; }

permutation "s1_begin" "s1_create_type" "s2_drop_schema" "s1_commit"
permutation "s1_begin" "s1_create_agg" "s2_drop_schema" "s1_commit"
permutation "s1_begin" "s1_create_func" "s2_drop_schema" "s1_commit"
permutation "s1_begin" "s1_create_op" "s2_drop_schema" "s1_commit"
permutation "s1_begin" "s1_create_opfamily" "s2_drop_schema" "s1_commit"
permutation "s1_begin" "s1_create_opclass" "s2_drop_schema" "s1_commit"
