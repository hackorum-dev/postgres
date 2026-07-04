/* contrib/valgrind_shmem/valgrind_shmem--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION valgrind_shmem" to load this file. \quit

CREATE FUNCTION valgrind_shmem_main_get(int) RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION valgrind_shmem_shm_toc_fg_get(int) RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION valgrind_shmem_shm_toc_bg_get(int) RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
