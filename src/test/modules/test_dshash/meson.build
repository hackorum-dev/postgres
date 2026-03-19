# Copyright (c) 2024-2026, PostgreSQL Global Development Group

test_dshash_sources = files(
  'test_dshash.c',
)

if host_system == 'windows'
  test_dshash_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_dshash',
    '--FILEDESC', 'test_dshash - test code for dshash',])
endif

test_dshash = shared_module('test_dshash',
  test_dshash_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_dshash

test_install_data += files(
  'test_dshash.control',
  'test_dshash--1.0.sql',
)

tests += {
  'name': 'test_dshash',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_dshash',
    ],
  },
}
