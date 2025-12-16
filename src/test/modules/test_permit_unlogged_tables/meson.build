# Copyright (c) 2022-2025, PostgreSQL Global Development Group

tests += {
  'name': 'test_permit_unlogged_tables',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_permit_unlogged_tables',
    ],
    'regress_args': ['--temp-config', '@0@/test.conf'.format(meson.current_source_dir())],
  },
  'tap': {
    'tests': [
      't/001_permit_unlogged_tables.pl',
    ],
  },
}
