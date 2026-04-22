# Copyright (c) 2024-2026, PostgreSQL Global Development Group

test_wal_record_stats_sources = files(
  'test_wal_record_stats.c',
)

if host_system == 'windows'
  test_wal_record_stats_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_wal_record_stats',
    '--FILEDESC', 'test_wal_record_stats - test WAL record stats from WAL buffers',])
endif

test_wal_record_stats = shared_module('test_wal_record_stats',
  test_wal_record_stats_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_wal_record_stats

test_install_data += files(
  'test_wal_record_stats.control',
  'test_wal_record_stats--1.0.sql',
)

