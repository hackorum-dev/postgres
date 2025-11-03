-- tcpinfo--1.0.sql
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_tcpinfo" to load this file. \quit

CREATE FUNCTION pg_stat_get_tcpinfo(
    OUT pid integer,
    OUT uid integer,
    OUT src_addr inet,
    OUT src_port integer,
    OUT dst_addr inet,
    OUT dst_port integer,
    OUT state text,
    OUT recvq integer,
    OUT sendq integer,
    OUT tcpinfo jsonb
)
RETURNS SETOF record
AS '$libdir/pg_stat_tcpinfo', 'pg_stat_get_tcpinfo'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_stat_get_tcpinfo()
IS 'Shows detailed TCP connection information on Linux.';

CREATE VIEW pg_stat_tcpinfo AS SELECT * FROM pg_stat_get_tcpinfo();

GRANT EXECUTE ON FUNCTION pg_stat_get_tcpinfo() TO pg_monitor;
GRANT EXECUTE ON FUNCTION pg_stat_get_tcpinfo() TO pg_read_all_stats;
GRANT SELECT ON pg_stat_tcpinfo TO pg_monitor;
GRANT SELECT ON pg_stat_tcpinfo TO pg_read_all_stats;
