# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_heapam_sources = files(
  'heappage_craft.c',
  'test_heapam.c',
)

if host_system == 'windows'
  test_heapam_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_heapam',
    '--FILEDESC', 'test_heapam - test code for heap AM',])
endif

test_heapam = shared_module('test_heapam',
  test_heapam_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_heapam

test_install_data += files(
  'test_heapam.control',
  'test_heapam--1.0.sql',
)

tests += {
  'name': 'test_heapam',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'move_in_out',
      'pruning',
    ],
  },
}
