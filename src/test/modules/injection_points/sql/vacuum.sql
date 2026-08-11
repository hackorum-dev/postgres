-- Tests for VACUUM

CREATE EXTENSION injection_points;

SELECT injection_points_set_local();
SELECT injection_points_attach('vacuum-index-cleanup-auto', 'notice');
SELECT injection_points_attach('vacuum-index-cleanup-disabled', 'notice');
SELECT injection_points_attach('vacuum-index-cleanup-enabled', 'notice');
SELECT injection_points_attach('vacuum-truncate-auto', 'notice');
SELECT injection_points_attach('vacuum-truncate-disabled', 'notice');
SELECT injection_points_attach('vacuum-truncate-enabled', 'notice');

-- Check state of index_cleanup and truncate in VACUUM.
CREATE TABLE vac_tab_on_toast_off(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=true, toast.vacuum_index_cleanup=false,
   vacuum_truncate=true, toast.vacuum_truncate=false);
CREATE TABLE vac_tab_off_toast_on(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=false, toast.vacuum_index_cleanup=true,
   vacuum_truncate=false, toast.vacuum_truncate=true);
-- Multiple relations should use their options in isolation.
VACUUM vac_tab_on_toast_off, vac_tab_off_toast_on;

-- Check "auto" case of index_cleanup and "truncate" controlled by
-- its GUC.
CREATE TABLE vac_tab_auto(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=auto, toast.vacuum_index_cleanup=auto);
SET vacuum_truncate = false;
VACUUM vac_tab_auto;
SET vacuum_truncate = true;
VACUUM vac_tab_auto;
RESET vacuum_truncate;

-- TOAST table inherits main table's resolved values
CREATE TABLE vac_tab_toast_inherit(i int, j text STORAGE EXTERNAL) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=false,
   autovacuum_vacuum_insert_threshold=1,
   autovacuum_vacuum_insert_scale_factor=0,
   vacuum_truncate=false, toast.vacuum_truncate=true);
VACUUM vac_tab_toast_inherit;
INSERT INTO vac_tab_toast_inherit
  VALUES (1, repeat('a', 10000)), (2, repeat('b', 10000));
SELECT pg_stat_force_next_flush();
SELECT s.vacuum_insert_score > 1 AS over
  FROM pg_class c, pg_stat_autovacuum_scores s
  WHERE s.relid = c.reltoastrelid AND c.relname = 'vac_tab_toast_inherit';

DROP TABLE vac_tab_auto;
DROP TABLE vac_tab_on_toast_off;
DROP TABLE vac_tab_off_toast_on;
DROP TABLE vac_tab_toast_inherit;

-- Cleanup
SELECT injection_points_detach('vacuum-index-cleanup-auto');
SELECT injection_points_detach('vacuum-index-cleanup-disabled');
SELECT injection_points_detach('vacuum-index-cleanup-enabled');
SELECT injection_points_detach('vacuum-truncate-auto');
SELECT injection_points_detach('vacuum-truncate-disabled');
SELECT injection_points_detach('vacuum-truncate-enabled');
DROP EXTENSION injection_points;
