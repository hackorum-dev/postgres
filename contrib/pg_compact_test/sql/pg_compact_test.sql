CREATE EXTENSION pg_compact_test;

--
-- Exercise RelationGetSpecificBufferForTuple via pg_test_compact_buffer().
--
-- A fillfactor=50 table guarantees ample free space on every page, so a
-- small tuple_size is certain to fit regardless of the compiled block size.
--

CREATE TABLE compacttest (id int, payload text) WITH (fillfactor = 50);
INSERT INTO compacttest
SELECT g, repeat('x', 200) FROM generate_series(1, 1000) g;

-- Sanity: at least a few pages so source_block (5) is in range.
SELECT pg_relation_size('compacttest') / current_setting('block_size')::int >= 6
  AS has_enough_pages;

-- Success path: a small tuple fits on a half-empty page.
SELECT pg_test_compact_buffer('compacttest', 0, 5, 200) AS small_tuple_fits;

--
-- Argument validation.  We use sqlstate verbosity for the messages whose
-- wording embeds runtime-dependent values like the relation page count.
--

-- Same source/target page is rejected.
SELECT pg_test_compact_buffer('compacttest', 3, 3, 200);

-- Non-positive tuple_size is rejected.
SELECT pg_test_compact_buffer('compacttest', 0, 5, 0);

-- Out-of-range block.  Use sqlstate verbosity because the message embeds
-- the table's current page count.
\set VERBOSITY sqlstate
SELECT pg_test_compact_buffer('compacttest', 0, 999999, 200);
\set VERBOSITY default

-- Wrong relkind: an index is not a heap.
CREATE INDEX compacttest_idx ON compacttest (id);
SELECT pg_test_compact_buffer('compacttest_idx', 0, 1, 100);

DROP TABLE compacttest;

--
-- Exercise heap_relocate end-to-end via pg_test_relocate_tuple(): move a
-- tuple from a tail page to page 0 and verify heap-level invariants.
--
-- Note: this wrapper does not update indexes (that responsibility belongs
-- to the VACUUM (COMPACT) orchestrator), so the test uses only TID-based
-- queries and sequential scans, which see the relocated row directly.
--

CREATE TABLE relocate_test (id int, payload text) WITH (fillfactor = 50);
INSERT INTO relocate_test
SELECT g, repeat('y', 200) FROM generate_series(1, 1000) g;

-- Snapshot the row that lives physically last in the table.
SELECT ctid AS rel_source_ctid
  FROM relocate_test
  ORDER BY ctid DESC
  LIMIT 1
\gset

-- Source row really is past page 0.
SELECT (:'rel_source_ctid'::tid::text NOT LIKE '(0,%)') AS source_past_page_0;

-- Relocate the tuple onto page 0.  Returns the new TID (NULL = skipped).
SELECT pg_test_relocate_tuple('relocate_test',
                              :'rel_source_ctid'::tid,
                              0) AS rel_new_tid
\gset

-- New TID is on page 0.
SELECT :'rel_new_tid' LIKE '(0,%)' AS new_tid_on_page_0;

-- The TID changed.
SELECT (:'rel_source_ctid'::tid <> :'rel_new_tid'::tid) AS tid_changed;

-- Row count is unchanged via sequential scan: no duplicates, no rows lost.
SELECT count(*) AS row_count FROM relocate_test;

-- The relocated row is reachable at the new TID exactly once.
SELECT count(*) AS at_new_tid
  FROM relocate_test
  WHERE ctid = :'rel_new_tid'::tid;

-- The old TID no longer holds a live row.
SELECT count(*) AS old_tid_dead
  FROM relocate_test
  WHERE ctid = :'rel_source_ctid'::tid;

DROP TABLE relocate_test;
DROP EXTENSION pg_compact_test;
