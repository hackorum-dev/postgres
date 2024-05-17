/* contrib/btree_gist/btree_gist--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.9'" to load this file. \quit

ALTER OPERATOR FAMILY gist_oid_ops USING gist ADD
	FUNCTION 11 (oid, oid) btoidsortsupport (internal) ;

ALTER OPERATOR FAMILY gist_int2_ops USING gist ADD
	FUNCTION 11 (int2, int2) btint2sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_int4_ops USING gist ADD
	FUNCTION 11 (int4, int4) btint4sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_int8_ops USING gist ADD
	FUNCTION 11 (int8, int8) btint8sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_float4_ops USING gist ADD
	FUNCTION 11 (float4, float4) btfloat4sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_float8_ops USING gist ADD
	FUNCTION 11 (float8, float8) btfloat8sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_timestamp_ops USING gist ADD
	FUNCTION 11 (timestamp, timestamp) timestamp_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_timestamptz_ops USING gist ADD
	FUNCTION 11 (timestamptz, timestamptz) timestamp_sortsupport (internal) ;

-- ALTER OPERATOR FAMILY gist_time_ops USING gist ADD
-- 	FUNCTION 12 (time, time) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_date_ops USING gist ADD
	FUNCTION 11 (date, date) date_sortsupport (internal) ;

-- ALTER OPERATOR FAMILY gist_interval_ops USING gist ADD
-- 	FUNCTION 12 (interval, interval) gist_stratnum_btree (int2) ;

-- ALTER OPERATOR FAMILY gist_cash_ops USING gist ADD
-- 	FUNCTION 12 (money, money) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_macaddr_ops USING gist ADD
	FUNCTION 11 (macaddr, macaddr) macaddr_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_text_ops USING gist ADD
	FUNCTION 11 (text, text) bttextsortsupport (internal) ;

ALTER OPERATOR FAMILY gist_bpchar_ops USING gist ADD
	FUNCTION 11 (bpchar, bpchar) bpchar_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_bytea_ops USING gist ADD
	FUNCTION 11 (bytea, bytea) bytea_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_numeric_ops USING gist ADD
	FUNCTION 11 (numeric, numeric) numeric_sortsupport (internal) ;

-- ALTER OPERATOR FAMILY gist_bit_ops USING gist ADD
--	FUNCTION 12 (bit, bit) gist_stratnum_btree (int2) ;

-- ALTER OPERATOR FAMILY gist_vbit_ops USING gist ADD
--	FUNCTION 12 (varbit, varbit) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_inet_ops USING gist ADD
	FUNCTION 11 (inet, inet) network_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_cidr_ops USING gist ADD
	FUNCTION 11 (cidr, cidr) network_sortsupport (internal) ;

--ALTER OPERATOR FAMILY gist_timetz_ops USING gist ADD
--	FUNCTION 12 (timetz, timetz) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_uuid_ops USING gist ADD
	FUNCTION 11 (uuid, uuid) uuid_sortsupport (internal) ;

-- ALTER OPERATOR FAMILY gist_macaddr8_ops USING gist ADD
-- 	FUNCTION 12 (macaddr8, macaddr8) gist_stratnum_btree (int2) ;

-- ALTER OPERATOR FAMILY gist_enum_ops USING gist ADD
-- 	FUNCTION 12 (anyenum, anyenum) gist_stratnum_btree (int2) ;

-- ALTER OPERATOR FAMILY gist_bool_ops USING gist ADD
-- 	FUNCTION 12 (bool, bool) gist_stratnum_btree (int2) ;
