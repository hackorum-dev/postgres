# Copyright (c) 2026, PostgreSQL Global Development Group

# Property-based test harness, built only when libhegel is available
# (-Dprop_tests=enabled; see the top-level meson.build).  All areas link into
# one binary, pg_prop, whose first argument selects the area:
#   pg_prop <area> [options] [property ...].
# t/001_prop.pl runs each area against a throwaway server it starts.
#
# This is the initial curated set: a handful of single-table areas with sound,
# fully reproducible oracles (round-trips and algebraic laws).  Further areas
# are added here as they land.

if not prop_tests_enabled
  subdir_done()
endif

prop_area_sources = files(
  'prop_base64.c',
  'prop_bytea.c',
  'prop_uuid.c',
  'prop_numeric.c',
  'prop_intervalio.c',
  'prop_jsonbroundtrip.c',
  'prop_prng.c',
)

pg_prop = executable('pg_prop',
  files('pg_prop.c', 'pg_prop_sql.c') + prop_area_sources,
  include_directories: include_directories('.'),
  dependencies: [frontend_code, libpq, libhegel],
  kwargs: default_bin_args + {
    'install': false,
  },
)
testprep_targets += pg_prop

# Register the Perl TAP driver only when TAP is available.  Without TAP the
# pg_prop binary still builds and can be run directly.
if prop_tests_tap
  tests += {
    'name': 'prop',
    'sd': meson.current_source_dir(),
    'bd': meson.current_build_dir(),
    'tap': {
      'tests': [
        't/001_prop.pl',
      ],
      'deps': [pg_prop],
    },
  }
endif
