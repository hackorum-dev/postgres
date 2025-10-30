# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_utf8_validate_sources = files(
  'test_utf8_validate.c',
)

if host_system == 'windows'
  test_utf8_validate_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_utf8_validate',
    '--FILEDESC', 'test_utf8_validate',])
endif

test_utf8_validate = shared_module('test_utf8_validate',
  test_utf8_validate_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += test_utf8_validate

install_data(
  'test_utf8_validate--1.0.sql',
  'test_utf8_validate.control',
  kwargs: contrib_data_args,
)
