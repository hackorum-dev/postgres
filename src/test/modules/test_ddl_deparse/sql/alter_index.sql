--
-- ALTER_INDEX
--

CREATE TABLE alter_idx_t (a int);
CREATE INDEX alter_idx_t_a_idx ON alter_idx_t (a);

ALTER INDEX alter_idx_t_a_idx INVISIBLE;
ALTER INDEX alter_idx_t_a_idx VISIBLE;

-- IF EXISTS form, on a missing index: no DDL command fires
ALTER INDEX IF EXISTS alter_idx_t_does_not_exist INVISIBLE;

-- Recursive form on a partitioned index
CREATE TABLE alter_idx_part (a int) PARTITION BY RANGE (a);
CREATE TABLE alter_idx_part_p1
    PARTITION OF alter_idx_part FOR VALUES FROM (0) TO (100);
CREATE INDEX alter_idx_part_a_idx ON alter_idx_part (a);

ALTER INDEX alter_idx_part_a_idx INVISIBLE;
ALTER INDEX alter_idx_part_a_idx VISIBLE;

DROP TABLE alter_idx_part;
DROP TABLE alter_idx_t;
