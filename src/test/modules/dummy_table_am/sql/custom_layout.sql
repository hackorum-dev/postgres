-- Tests for a table AM whose amoptions callback returns a layout of its
-- own, without a StdRdOptions prefix (has_std_options_prefix = false).
CREATE EXTENSION dummy_table_am;

-- Standard heap options are not accepted by this AM.
CREATE TABLE dct_bad (a int) USING dummy_custom_table_am WITH (fillfactor = 50);

-- TOAST table creation must use the default toast_value_type rather than
-- reading StdRdOptions.toast_value_type out of rd_options: option_c sits at
-- that offset, and 0 is not a valid toast_value_type while 2 would select
-- oid8.
CREATE TABLE dct_default (a int, b text) USING dummy_custom_table_am;
CREATE TABLE dct_c0 (a int, b text) USING dummy_custom_table_am
    WITH (option_c = 0);
CREATE TABLE dct_c2 (a int, b text) USING dummy_custom_table_am
    WITH (option_a = 10, option_b = 20, option_c = 2);
SELECT c.relname, c.reloptions, a.atttypid::regtype AS chunk_id_type
    FROM pg_class c
    JOIN pg_attribute a ON a.attrelid = c.reltoastrelid AND a.attname = 'chunk_id'
    WHERE c.relname IN ('dct_default', 'dct_c0', 'dct_c2')
    ORDER BY c.relname;

-- VACUUM hands the main table's storage parameters down to its TOAST
-- table; that must not copy a StdRdOptions out of this AM's smaller
-- rd_options.
INSERT INTO dct_c2
    SELECT 1, string_agg(md5(i::text), '') FROM generate_series(1, 500) i;
SELECT pg_relation_size(reltoastrelid) > 0 AS has_toast_data
    FROM pg_class WHERE oid = 'dct_c2'::regclass;
VACUUM dct_c2;
SELECT a, length(b) FROM dct_c2;

-- Switching to heap revalidates the options against heap's parser.
ALTER TABLE dct_c2 SET ACCESS METHOD heap;
ALTER TABLE dct_c2 SET ACCESS METHOD heap, SET (fillfactor = 50),
    RESET (option_a, option_b, option_c);
SELECT (SELECT amname FROM pg_am WHERE oid = relam) AS amname, reloptions
    FROM pg_class WHERE oid = 'dct_c2'::regclass;
SELECT a, length(b) FROM dct_c2;

DROP TABLE dct_default;
DROP TABLE dct_c0;
DROP TABLE dct_c2;

DROP EXTENSION dummy_table_am;
