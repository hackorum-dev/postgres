# Copyright (c) 2024-2026, PostgreSQL Global Development Group

buftable_bench_sources = files(
  'buftable_bench.c',
)

if host_system == 'windows'
  buftable_bench_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'buftable_bench',
    '--FILEDESC', 'buftable_bench - rdtsc micro-benchmark for the buffer mapping table',])
endif

buftable_bench = shared_module('buftable_bench',
  buftable_bench_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += buftable_bench

test_install_data += files(
  'buftable_bench.control',
  'buftable_bench--1.0.sql',
)
