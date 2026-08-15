-- Rescanning a GiST index scan must not carry pending killed-item offsets
-- over into the next scan.  The offsets in GISTScanOpaque->killedItems are
-- only meaningful for the page in ->curBlkno, and the next scan starts by
-- reading the root page.
CREATE TABLE gist_rescan(k int, p point) WITH (autovacuum_enabled = false);
INSERT INTO gist_rescan
    SELECT g, point(g, i)
    FROM generate_series(1, 200) g, generate_series(1, 60) i;
CREATE INDEX ON gist_rescan USING gist (p);
-- leave one live row per group, behind 59 dead ones for killtuples to find
DELETE FROM gist_rescan WHERE (p)[1] < 60;

SET enable_seqscan = false;
SET enable_bitmapscan = false;

-- Stops each inner scan early, so every rescan has killed items pending.
-- count(s.k) rather than count(*), so that this is not an index-only scan.
SELECT count(s.k) FROM generate_series(1, 200) o(g),
    LATERAL (SELECT k FROM gist_rescan
             WHERE p <@ box(point(o.g - 0.5, -1000), point(o.g + 0.5, 1000))
             LIMIT 1) s;

-- every row must still be reachable through the index
SELECT count(*) FROM gist_rescan
WHERE p <@ box(point(0, 0), point(1000, 1000));

DROP TABLE gist_rescan;
