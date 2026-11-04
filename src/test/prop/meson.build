# Copyright (c) 2026, PostgreSQL Global Development Group

# Property-based test harness, built only when libhegel is available
# (-Dprop_tests=enabled; see the top-level meson.build).  All areas link into
# one binary, pg_prop, whose first argument selects the area:
#   pg_prop <area> [options] [property ...].
# t/001_prop.pl runs each area against a throwaway server it starts.
#
# The initial set is a few strong examples in high-value areas, each with a
# sound oracle (a model, an invariant, or a differential check):
#   binaryheap  -- a data-structure model test on src/common/binaryheap.c
#   nbtree      -- an index-scan vs sequential-scan plan-results differential
#   isolation   -- transaction isolation guarantees across concurrent backends
#                  (write-skew, snapshot stability, phantoms, deadlock victim)
# prng is an in-process self-test of the generator.  Further areas are added
# here as they land.

if not prop_tests_enabled
  subdir_done()
endif

prop_area_sources = files(
  'prop_binaryheap.c',
  'prop_nbtree.c',
  'sql/prop_isolation.c',
  'prop_prng.c',
)

pg_prop = executable('pg_prop',
  files('pg_prop.c', 'pg_prop_sql.c') + prop_area_sources,
  include_directories: include_directories('.', 'sql'),
  dependencies: [frontend_code, libpq, libhegel],
  kwargs: default_bin_args + {
    'install': false,
  },
)
testprep_targets += pg_prop

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
