# Copyright (c) 2026, PostgreSQL Global Development Group

test_lossy_gist_ops_sources = files(
  'test_lossy_gist_ops.c',
)

if host_system == 'windows'
  test_lossy_gist_ops_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_lossy_gist_ops',
    '--FILEDESC', 'test_lossy_gist_ops - test opclass for a lossy GiST index',])
endif

test_lossy_gist_ops = shared_module('test_lossy_gist_ops',
  test_lossy_gist_ops_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_lossy_gist_ops

test_install_data += files(
  'test_lossy_gist_ops.control',
  'test_lossy_gist_ops--1.0.sql',
)
