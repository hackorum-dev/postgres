# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not ssl.found()
  subdir_done()
endif

test_tde_sources = files(
  'test_tde.c',
)

if host_system == 'windows'
  test_tde_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_tde',
    '--FILEDESC', 'test_tde - reference implementation for I/O transform hooks',])
endif

test_tde = shared_module('test_tde',
  test_tde_sources,
  kwargs: contrib_mod_args + {
    'dependencies': [ssl, contrib_mod_args['dependencies']]
  },
)
contrib_targets += test_tde

tests += {
  'name': 'test_tde',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'basic',
    ],
    'regress_args': ['--temp-config', files('test_tde.conf')],
    # Disabled because these tests require "shared_preload_libraries=test_tde"
    'runningcheck': false,
  },
}
