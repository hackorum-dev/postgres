# Copyright (c) 2026, PostgreSQL Global Development Group

pg_inplace_upgrade_sources = files(
  'pg_inplace_upgrade.c',
)

if host_system == 'windows'
  pg_inplace_upgrade_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_inplace_upgrade',
    '--FILEDESC', 'pg_inplace_upgrade - upgrade a database cluster in place',])
endif

pg_inplace_upgrade = executable('pg_inplace_upgrade',
  pg_inplace_upgrade_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)
bin_targets += pg_inplace_upgrade

tests += {
  'name': 'pg_inplace_upgrade',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/002_prepare.pl',
      't/007_posture.pl',
    ],
  },
}
