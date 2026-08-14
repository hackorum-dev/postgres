# Copyright (c) 2026, PostgreSQL Global Development Group

test_checksums_sources = files(
	'test_checksums.c',
)

test_checksums = shared_module('test_checksums',
	test_checksums_sources,
	kwargs: pg_test_mod_args,
)
test_install_libs += test_checksums

test_install_data += files(
	'test_checksums.control',
	'test_checksums--1.0.sql',
)

tests += {
  'name': 'test_checksums',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
	},
    'tests': [
      't/001_basic.pl',
      't/002_restarts.pl',
      't/003_standby_restarts.pl',
      't/004_offline.pl',
      't/005_injection.pl',
      't/006_pgbench_single.pl',
      't/007_pgbench_standby.pl',
      't/008_pitr.pl',
      't/009_fpi.pl',
      't/010_backup_straddle.pl',
      't/011_standby_straddle.pl',
      't/012_offline_standby.pl',
      't/013_rewind.pl',
      't/014_lockstep.pl',
      't/015_backup_online_enable.pl',
      't/016_backup_from_standby.pl',
      't/017_standby_crash_after_disable.pl',
      't/018_promote_enable_crash.pl',
      't/019_restartpoint_race.pl',
      't/020_primary_enable_crash.pl',
      't/021_standby_shutdown_catchup.pl',
      't/022_resident_enable_crash.pl',
      't/023_concurrent_checkpoint_enable.pl',
      't/024_enable_crash_after_checkpoint.pl',
      't/025_cascade_divergence.pl',
      't/029_checkpoint_transition_race.pl',
      't/030_offline_survives_rereplay.pl',
      't/031_rewind_offline_enable.pl',
    ],
  },
}
