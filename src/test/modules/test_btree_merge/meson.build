# Copyright (c) 2026, PostgreSQL Global Development Group

test_btree_merge_sources = files(
  'test_btree_merge.c',
)

if host_system == 'windows'
  test_btree_merge_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_btree_merge',
    '--FILEDESC', 'test_btree_merge - test code for btree merge scan',])
endif

test_btree_merge = shared_module('test_btree_merge',
  test_btree_merge_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_btree_merge

test_install_data += files(
  'test_btree_merge.control',
  'test_btree_merge--1.0.sql',
)

tests += {
  'name': 'test_btree_merge',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_btree_merge',
    ],
  },
}
