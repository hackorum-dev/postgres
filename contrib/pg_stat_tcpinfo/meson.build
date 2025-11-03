# Copyright (c) 2025, PostgreSQL Global Development Group

pg_stat_tcpinfo_sources = files(
  'pg_stat_tcpinfo.c',
)

pg_stat_tcpinfo = shared_module('pg_stat_tcpinfo',
  pg_stat_tcpinfo_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_stat_tcpinfo

install_data(
  'pg_stat_tcpinfo.control',
  'pg_stat_tcpinfo--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_stat_tcpinfo',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
}
