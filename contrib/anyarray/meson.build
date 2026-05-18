# Copyright (c) 2026, PostgreSQL Global Development Group

anyarray_sources = files(
  'anyarray.c',
  'anyarray_bool.c',
  'anyarray_gin.c',
  'anyarray_gist.c',
  'anyarray_op.c',
)

if host_system == 'windows'
  anyarray_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'anyarray',
    '--FILEDESC', 'anyarray - operations and indexes for arrays of any type',])
endif

anyarray = shared_module('anyarray',
  anyarray_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += anyarray

install_data(
  'anyarray--1.0.sql',
  'anyarray.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'anyarray',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'anyarray',
    ],
  },
}
