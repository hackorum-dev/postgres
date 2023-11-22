# Copyright (c) 2022-2023, PostgreSQL Global Development Group

snowflake_sequence_sources = files(
  'snowflake_sequence.c',
)

if host_system == 'windows'
  snowflake_sequence_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'snowflake_sequence',
    '--FILEDESC', 'System-wide unique value generator',])
endif

snowflake_sequence = shared_module('snowflake_sequence',
  snowflake_sequence_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += snowflake_sequence

install_data(
  'snowflake_sequence.control',
  'snowflake_sequence--1.0.sql',
  kwargs: contrib_data_args,
)
