setup
{
    CREATE DOMAIN dd AS INT;
    ALTER DOMAIN dd ADD CONSTRAINT cc CHECK(VALUE > 1) NOT VALID;
}

teardown
{
    DROP DOMAIN IF EXISTS dd CASCADE;
}

session s1
step b1         { BEGIN; }
step s_add      { ALTER DOMAIN dd ADD CONSTRAINT dd_check1 CHECK(VALUE > 0); }
step s1_drop    { ALTER DOMAIN dd DROP DEFAULT; }
step s1_set     { ALTER DOMAIN dd SET NOT NULL; }
step s1_drop2   { ALTER DOMAIN dd DROP CONSTRAINT cc;}
step c1         { COMMIT; }

session s2
step b2         { BEGIN; }
step s2_set     { ALTER DOMAIN dd SET DEFAULT 3; }
step s2_drop    { DROP DOMAIN dd; }
step v2         { ALTER DOMAIN dd VALIDATE CONSTRAINT cc; }
step c2         { COMMIT; }

permutation b1 b2 s_add s2_drop c1 c2
permutation b1 b2 s_add s2_set c1 c2
permutation b1 b2 s1_set s1_drop s2_set c1 c2
permutation b1 b2 s1_drop s2_set c1 c2
permutation b1 b2 s1_set s1_drop2 v2 c1 c2
permutation b1 b2 v2 s1_drop2  c2 c1
