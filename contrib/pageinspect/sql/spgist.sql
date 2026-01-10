-- The gist_page_opaque_info() function prints the page's LSN.
-- Use an unlogged index, so that the LSN is predictable.
CREATE UNLOGGED TABLE test_gist AS SELECT point(i,i) p, i::text t FROM
    generate_series(1,1000) i;
CREATE INDEX test_spgist_idx ON test_gist USING spgist (p);

-- Page 0 is the root, the rest are leaf pages
SELECT * FROM spgist_page_opaque_info(get_raw_page('test_spgist_idx', 0));
SELECT * FROM spgist_page_opaque_info(get_raw_page('test_spgist_idx', 1));
SELECT * FROM spgist_page_opaque_info(get_raw_page('test_spgist_idx', 2));

-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes and architectures.
\set VERBOSITY terse

-- Failure with various modes.
-- invalid page size
SELECT spgist_page_opaque_info('aaa'::bytea);
-- invalid special area size
SELECT * FROM spgist_page_opaque_info(get_raw_page('test_gist', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT spgist_page_opaque_info(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test_gist;
