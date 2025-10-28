-- Test GUCs for predicate lock ratios

-- Check that new GUC variables exist and have default values
SHOW max_predicate_locks_per_relation_ratio;
SHOW max_predicate_locks_per_page_ratio;

-- Test setting the GUCs to valid values
SET max_predicate_locks_per_relation_ratio = 0.1;
SHOW max_predicate_locks_per_relation_ratio;

SET max_predicate_locks_per_page_ratio = 0.5;
SHOW max_predicate_locks_per_page_ratio;

-- Test boundary values
SET max_predicate_locks_per_relation_ratio = 0.0;  -- should disable
SET max_predicate_locks_per_relation_ratio = 1.0;  -- max value

-- Test invalid values (should fail)
SET max_predicate_locks_per_relation_ratio = -0.1;
SET max_predicate_locks_per_page_ratio = 1.1;

-- Reset to defaults
RESET max_predicate_locks_per_relation_ratio;
RESET max_predicate_locks_per_page_ratio;
