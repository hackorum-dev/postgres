--
-- COMPACT
--

-- A bloated table whose live rows have drifted to the physical end.
-- After VACUUM, dead tuples are gone but the trailing pages still hold the
-- 1000 surviving rows.  COMPACT relocates them to low-numbered pages and
-- truncates the trailing empties in a single command.
CREATE TABLE compact_bloat (id int, payload text) WITH (fillfactor = 100);
INSERT INTO compact_bloat
    SELECT g, repeat('z', 200) FROM generate_series(1, 10000) g;
DELETE FROM compact_bloat WHERE id <= 9000;
VACUUM compact_bloat;

-- Snapshot of the physical-block range of live rows before compaction.
SELECT (min((ctid::text::point)[0]::int) > 64) AS rows_at_high_pages
  FROM compact_bloat;

-- Run COMPACT.
COMPACT compact_bloat;

-- Row count is preserved.
SELECT count(*) AS row_count FROM compact_bloat;

-- All surviving rows are now physically near the start.  This is the
-- defining outcome of the relocation pass and does not depend on whether
-- the truncation step was able to reclaim trailing pages (which in turn
-- depends on OldestXmin and so can be held back by concurrent activity
-- in a parallel test slot).
SELECT (max((ctid::text::point)[0]::int) < 64) AS rows_packed_to_low_pages
  FROM compact_bloat;

DROP TABLE compact_bloat;

-- Options-list form, with ANALYZE.
CREATE TABLE compact_opts (id int, payload text) WITH (fillfactor = 100);
INSERT INTO compact_opts
    SELECT g, repeat('q', 200) FROM generate_series(1, 2000) g;
DELETE FROM compact_opts WHERE id <= 1800;
VACUUM compact_opts;
COMPACT (ANALYZE) compact_opts;
SELECT count(*) FROM compact_opts;
DROP TABLE compact_opts;

-- Argument validation.
COMPACT (FOO) some_table;
