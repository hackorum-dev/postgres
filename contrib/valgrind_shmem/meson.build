# Copyright (c) 2022-2026, PostgreSQL Global Development Group

valgrind_shmem_sources = files(
  'valgrind_shmem.c',
)

valgrind_shmem = shared_module('valgrind_shmem',
  valgrind_shmem_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += valgrind_shmem

install_data(
  'valgrind_shmem--1.0.sql',
  'valgrind_shmem.control',
  kwargs: contrib_data_args,
)
