# Copyright (c) 2026, PostgreSQL Global Development Group

# The port getopt_long() is compiled in directly rather than taken from
# libpgport, which uses the system implementation where one exists.
test_getopt_long_sources = files(
  'test_getopt_long.c',
  '../../../port/getopt_long.c',
)

if host_system == 'windows'
  test_getopt_long_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_getopt_long',
    '--FILEDESC', 'standalone getopt_long tester',
  ])
endif

test_getopt_long = executable('test_getopt_long',
  test_getopt_long_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

# Same program linked against the system getopt_long(), where there is one,
# to check the port version behaves the same.
test_getopt_long_system = executable('test_getopt_long_system',
  files('test_getopt_long.c'),
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

tests += {
  'name': 'test_getopt_long',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_getopt_long.pl',
    ],
    'deps': [
      test_getopt_long,
      test_getopt_long_system,
    ],
  },
}
