# Copyright (c) 2022-2024, PostgreSQL Global Development Group

vacuum_stats_sources = files(
  'stats_history.c',
)

if host_system == 'windows'
  pg_stat_statements_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'stats_history',
    '--FILEDESC', 'stats_history - history of statistics from important maintenance tasks',])
endif

pg_stat_statements = shared_module('stats_history',
  pg_stat_statements_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_stat_statements

install_data(
  'stats_history.control',
  'stats_history--1.0.sql'
  kwargs: contrib_data_args,
)

tests += {
  'name': 'stats_history',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
    ],
    'regress_args': ['--temp-config', files('stats_history.conf')],
    # Disabled because these tests require
    # "shared_preload_libraries=pg_stat_statements", which typical
    # runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
  'tap': {
    'tests': [
    ],
  },
}
