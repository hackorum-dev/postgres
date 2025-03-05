# Copyright (c) 2025, PostgreSQL Global Development Group

workmem_sources = files(
  'workmem.c',
)

if host_system == 'windows'
  workmem_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'workmem',
    '--FILEDESC', 'workmem - extension that adjusts PostgreSQL work_mem per node',])
endif

workmem = shared_module('workmem',
  workmem_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += workmem

tests += {
  'name': 'workmem',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'workmem',
    ],
  },
}
