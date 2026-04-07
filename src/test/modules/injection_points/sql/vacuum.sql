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
CREATE TABLE vac_tab_on_toast(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=true,
   vacuum_truncate=true);
CREATE TABLE vac_tab_off_toast(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=false,
   vacuum_truncate=false);
-- Multiple relations should use their options in isolation.
VACUUM vac_tab_on_toast, vac_tab_off_toast;

-- Check "auto" case of index_cleanup and "truncate" controlled by
-- its GUC.
CREATE TABLE vac_tab_auto(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=auto);
SET vacuum_truncate = false;
VACUUM vac_tab_auto;
SET vacuum_truncate = true;
VACUUM vac_tab_auto;
RESET vacuum_truncate;

DROP TABLE vac_tab_auto;
DROP TABLE vac_tab_on_toast;
DROP TABLE vac_tab_off_toast;

-- Cleanup
SELECT injection_points_detach('vacuum-index-cleanup-auto');
SELECT injection_points_detach('vacuum-index-cleanup-disabled');
SELECT injection_points_detach('vacuum-index-cleanup-enabled');
SELECT injection_points_detach('vacuum-truncate-auto');
SELECT injection_points_detach('vacuum-truncate-disabled');
SELECT injection_points_detach('vacuum-truncate-enabled');
DROP EXTENSION injection_points;
