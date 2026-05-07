# Copyright (c) 2026, PostgreSQL Global Development Group

pg_compact_test_sources = files(
  'pg_compact_test.c',
)

if host_system == 'windows'
  pg_compact_test_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_compact_test',
    '--FILEDESC', 'pg_compact_test - exercise heap compaction primitives',])
endif

pg_compact_test = shared_module('pg_compact_test',
  pg_compact_test_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_compact_test

install_data(
  'pg_compact_test--1.0.sql',
  'pg_compact_test.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_compact_test',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_compact_test',
    ],
  },
}
