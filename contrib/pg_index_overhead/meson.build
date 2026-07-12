# Copyright (c) 2026, PostgreSQL Global Development Group

pg_index_overhead_sources = files(
  'pg_index_overhead.c',
)

if host_system == 'windows'
  pg_index_overhead_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_index_overhead',
    '--FILEDESC', 'pg_index_overhead - per-index maintenance instrumentation for EXPLAIN',])
endif

pg_index_overhead = shared_module('pg_index_overhead',
  pg_index_overhead_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_index_overhead
