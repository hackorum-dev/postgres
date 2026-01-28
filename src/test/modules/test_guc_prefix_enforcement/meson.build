# Copyright (c) 2026, PostgreSQL Global Development Group

# Main test module (good behavior)
test_guc_prefix_enforcement_sources = files(
  'test_guc_prefix_enforcement.c',
)

if host_system == 'windows'
  test_guc_prefix_enforcement_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc_prefix_enforcement',
    '--FILEDESC', 'test_guc_prefix_enforcement - test module for GUC prefix reservation enforcement',])
endif

test_guc_prefix_enforcement = shared_module('test_guc_prefix_enforcement',
  test_guc_prefix_enforcement_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc_prefix_enforcement

# Test module without reservation
test_guc_no_reserve_sources = files(
  'test_guc_no_reserve.c',
)

if host_system == 'windows'
  test_guc_no_reserve_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc_no_reserve',
    '--FILEDESC', 'test_guc_no_reserve - test module without prefix reservation',])
endif

test_guc_no_reserve = shared_module('test_guc_no_reserve',
  test_guc_no_reserve_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc_no_reserve

# Test module with wrong prefix
test_guc_wrong_prefix_sources = files(
  'test_guc_wrong_prefix.c',
)

if host_system == 'windows'
  test_guc_wrong_prefix_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc_wrong_prefix',
    '--FILEDESC', 'test_guc_wrong_prefix - test module with wrong prefix reservation',])
endif

test_guc_wrong_prefix = shared_module('test_guc_wrong_prefix',
  test_guc_wrong_prefix_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc_wrong_prefix

# Test module with variable without prefix (no dot)
test_guc_no_prefix_sources = files(
  'test_guc_no_prefix.c',
)

if host_system == 'windows'
  test_guc_no_prefix_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_guc_no_prefix',
    '--FILEDESC', 'test_guc_no_prefix - test module with variable without prefix',])
endif

test_guc_no_prefix = shared_module('test_guc_no_prefix',
  test_guc_no_prefix_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_guc_no_prefix

tests += {
  'name': 'test_guc_prefix_enforcement',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_prefix_enforcement.pl',
    ],
  },
}
