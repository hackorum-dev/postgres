# Copyright (c) 2022-2024, PostgreSQL Global Development Group

alphabet_join_sources = files(
  'alphabet_join.c',
)

alphabet_join = shared_module('alphabet_join',
  alphabet_join_sources,
  kwargs: contrib_mod_args,
)

contrib_targets += alphabet_join
