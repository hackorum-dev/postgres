--
-- VACUUM must not unlink a live posting tree page.
--
-- When a page split is left unfinished, the new right half is reachable
-- through the sibling chain but has no downlink in its parent.  VACUUM deletes
-- empty pages by walking downlinks, so it takes the incompletely split page
-- for the left sibling of the page it is deleting, and relinks the chain over
-- the top of the live right half.  Every row indexed there becomes
-- unreachable.
--

CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;

-- Make injection points local to this process, for concurrency.
SELECT injection_points_set_local();

-- One key with many rows, so that it gets a posting tree with several leaves.
CREATE TABLE gin_orphan (id int, a int[]) WITH (autovacuum_enabled = off);
INSERT INTO gin_orphan SELECT g, '{1}'::int[] FROM generate_series(1, 120000) g;
CREATE INDEX gin_orphan_idx ON gin_orphan USING gin (a) WITH (fastupdate = off);

-- Free heap space in the middle, so that the inserts below reuse those line
-- pointers and split a middle leaf.  A split left unfinished at the rightmost
-- leaf would be harmless, as nothing is ever deleted to the right of it.
DELETE FROM gin_orphan WHERE id BETWEEN 40001 AND 80000;
VACUUM (INDEX_CLEANUP ON) gin_orphan;

-- Leave a split unfinished, as a crash or a cancelled insert would.
SELECT injection_points_attach('gin-leave-leaf-split-incomplete', 'error');
DO $$
DECLARE i int := 0;
BEGIN
  LOOP
    BEGIN
      INSERT INTO gin_orphan VALUES (1000000 + i, '{1}');
    EXCEPTION WHEN others THEN
      EXIT;
    END;
    i := i + 1;
    IF i > 200000 THEN
      RAISE 'no leaf split after % inserts', i;
    END IF;
  END LOOP;
END $$;
SELECT injection_points_detach('gin-leave-leaf-split-incomplete');

-- The page that was split, its right half, and the page after that.
CREATE TEMP TABLE geom AS
SELECT b AS splitblk,
       o.rightlink::int AS orphan,
       (gin_page_opaque_info(get_raw_page('gin_orphan_idx',
                                          o.rightlink::int))).rightlink::int AS victim
  FROM generate_series(0, (pg_relation_size('gin_orphan_idx') /
                           current_setting('block_size')::int)::int - 1) b,
       LATERAL gin_page_opaque_info(get_raw_page('gin_orphan_idx', b)) o
 WHERE o.flags @> '{incomplete_split}';

-- The tree has to take the shape the test needs, or the answers below could
-- come out right for uninteresting reasons.
SELECT count(*) = 1 AS one_unfinished_split,
       bool_and(orph.flags @> '{data,leaf}'
                AND NOT orph.flags @> '{deleted}') AS orphan_is_a_live_leaf,
       bool_and(vic.rightlink <> 4294967295) AS victim_is_not_rightmost,
       bool_and((SELECT count(*) > 0
                   FROM gin_leafpage_items(get_raw_page('gin_orphan_idx',
                                                        g.orphan)) i,
                        LATERAL unnest(i.tids) t
                  WHERE t IN (SELECT ctid FROM gin_orphan))) AS orphan_holds_rows
  FROM geom g,
       LATERAL gin_page_opaque_info(get_raw_page('gin_orphan_idx', g.orphan)) orph,
       LATERAL gin_page_opaque_info(get_raw_page('gin_orphan_idx', g.victim)) vic;

-- Empty exactly the page after the right half, so VACUUM deletes it.
DELETE FROM gin_orphan
 WHERE ctid IN (SELECT t
                  FROM geom g,
                       gin_leafpage_items(get_raw_page('gin_orphan_idx', g.victim)) i,
                       LATERAL unnest(i.tids) t);
VACUUM (INDEX_CLEANUP ON) gin_orphan;

-- The chain must still reach the right half.
SELECT (gin_page_opaque_info(get_raw_page('gin_orphan_idx', g.splitblk))).rightlink = g.orphan
         AS chain_still_reaches_the_right_half
  FROM geom g;

-- Every row left in the table has a = '{1}', so these two counts must agree.
SET enable_seqscan = off;
SELECT count(*) AS rows_found_by_the_index FROM gin_orphan WHERE a @> '{1}';
RESET enable_seqscan;

SELECT count(*) AS rows_in_the_table FROM gin_orphan;

DROP TABLE gin_orphan;
DROP EXTENSION pageinspect;
DROP EXTENSION injection_points;
