# Copyright (c) 2022-2024, PostgreSQL Global Development Group

hint_via_alias_sources = files(
  'hint_via_alias.c',
)

hint_via_alias = shared_module('hint_via_alias',
  hint_via_alias_sources,
  kwargs: contrib_mod_args,
)

contrib_targets += hint_via_alias
