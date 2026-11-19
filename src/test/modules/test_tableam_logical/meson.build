# Copyright (c) 2026, PostgreSQL Global Development Group

test_tableam_logical_sources = files(
  'test_tableam_logical.c',
)

if host_system == 'windows'
  test_tableam_logical_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_tableam_logical',
    '--FILEDESC', 'test_tableam_logical - table access methods with and without logical decoding support',])
endif

test_tableam_logical = shared_module('test_tableam_logical',
  test_tableam_logical_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_tableam_logical

test_install_data += files(
  'test_tableam_logical.control',
  'test_tableam_logical--1.0.sql',
)

tests += {
  'name': 'test_tableam_logical',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'repack',
    ],
    # Disabled because these tests require "wal_level=replica", which
    # some runningcheck users do not have (e.g. buildfarm clients).
    'regress_args': ['--temp-config', files('tableam_logical.conf')],
    'runningcheck': false,
  },
}
