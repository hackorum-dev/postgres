# Copyright (c) 2026, PostgreSQL Global Development Group

test_parallel_dml_safety_sources = files(
  'test_parallel_dml_safety.c',
)

if host_system == 'windows'
  test_parallel_dml_safety_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_parallel_dml_safety',
    '--FILEDESC', 'test_parallel_dml_safety - test code for parallel DML safety caching',])
endif

test_parallel_dml_safety = shared_module('test_parallel_dml_safety',
  test_parallel_dml_safety_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_parallel_dml_safety

test_install_data += files(
  'test_parallel_dml_safety.control',
  'test_parallel_dml_safety--1.0.sql',
)

tests += {
  'name': 'test_parallel_dml_safety',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'parallel_dml_safety',
    ],
  },
}
