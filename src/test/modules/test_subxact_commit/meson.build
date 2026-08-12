# Copyright (c) 2026, PostgreSQL Global Development Group

test_subxact_commit_sources = files(
  'test_subxact_commit.c',
)

if host_system == 'windows'
  test_subxact_commit_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_subxact_commit',
    '--FILEDESC', 'test_subxact_commit - force errors at subtransaction commit',])
endif

test_subxact_commit = shared_module('test_subxact_commit',
  test_subxact_commit_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_subxact_commit

test_install_data += files(
  'test_subxact_commit.control',
  'test_subxact_commit--1.0.sql',
)

tests += {
  'name': 'test_subxact_commit',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_subxact_resurrect_pk.pl',
    ],
  },
}
