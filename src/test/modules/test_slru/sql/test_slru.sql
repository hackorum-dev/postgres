CREATE EXTENSION test_slru;

-- how many pages can fit on one slru segment
SELECT current_setting('slru_pps')::INT AS slru_pps \gset

SELECT (:slru_pps * 6.8125) AS pageno \gset

-- count from zero
SELECT (:pageno + (:slru_pps * 1.5) - 1) AS pageno_max \gset

SELECT (:pageno_max - (:slru_pps / 2)) AS pageno_middle \gset

-- non existed page
SELECT test_slru_page_exists(:pageno::BIGINT);
SELECT test_slru_page_write(:pageno::BIGINT, 'Test SLRU');
SELECT test_slru_page_read(:pageno::BIGINT);
SELECT test_slru_page_exists(:pageno::BIGINT);

-- Generate extra pages. Redirect output into table because number of funciton
-- calls depends on the current value of slru_pps parameter.

CREATE TABLE slru_write_results (
		pageno BIGINT,
		write_count INTEGER
);

INSERT INTO slru_write_results (pageno, write_count)
SELECT a, count(test_slru_page_write(a::BIGINT, 'Test SLRU'))
FROM generate_series(:pageno, :pageno_max, 1) AS a
GROUP BY a;

DROP TABLE slru_write_results;

-- Reading page in buffer for read and write
SELECT test_slru_page_read(:pageno_middle::BIGINT, true);
-- Reading page in buffer for read-only
SELECT test_slru_page_readonly(:pageno_middle::BIGINT);
-- Reading page not in buffer with read-only
SELECT test_slru_page_readonly(:pageno::BIGINT);

-- Write all the pages in buffers
SELECT test_slru_page_writeall();
-- Flush the last page written out.
SELECT test_slru_page_sync(:pageno_max::BIGINT);
SELECT test_slru_page_exists(:pageno_max::BIGINT);
-- Segment deletion
SELECT test_slru_page_delete(:pageno_max::BIGINT);
SELECT test_slru_page_exists(:pageno_max::BIGINT);
-- Page truncation
SELECT test_slru_page_exists(:pageno_middle::BIGINT);
SELECT test_slru_page_truncate(:pageno_middle::BIGINT);
SELECT test_slru_page_exists(:pageno_middle::BIGINT);

-- Full deletion
SELECT test_slru_delete_all();
SELECT test_slru_page_exists(:pageno::BIGINT);
SELECT test_slru_page_exists(:pageno_middle::BIGINT);
SELECT test_slru_page_exists(:pageno_max::BIGINT);

--
-- Test 64-bit pages
--
\set no_such_page	0x1234500000000
-- second page in SLRU segments
\set pageno			0x1234500000001

-- count from zero
SELECT (:no_such_page + (:slru_pps * 1.5)) AS pageno_max \gset

SELECT (:pageno_max - (:slru_pps / 2)) AS pageno_middle \gset

SELECT test_slru_page_exists(:no_such_page);
SELECT test_slru_page_write(:no_such_page, 'Test SLRU 64-bit');
SELECT test_slru_page_read(:no_such_page);
SELECT test_slru_page_exists(:no_such_page);

-- Generate extra pages. Redirect output into table because number of funciton
-- calls depends on the current value of slru_pps parameter.

CREATE TABLE slru_write_results (
		pageno BIGINT,
		write_count INTEGER
);

INSERT INTO slru_write_results (pageno, write_count)
SELECT a, count(test_slru_page_write(a::BIGINT, 'Test SLRU 64-bit'))
FROM generate_series(:pageno, :pageno_max, 1) AS a
GROUP BY a;

DROP TABLE slru_write_results;

-- Reading page in buffer for read and write
SELECT test_slru_page_read(:pageno_middle::BIGINT, true);
-- Reading page in buffer for read-only
SELECT test_slru_page_readonly(:pageno_middle::BIGINT);
-- Reading page not in buffer with read-only
SELECT test_slru_page_readonly(:pageno);

-- Write all the pages in buffers
SELECT test_slru_page_writeall();
-- Flush the last page written out.
SELECT test_slru_page_sync(:pageno_max::BIGINT);
SELECT test_slru_page_exists(:pageno_max::BIGINT);
-- Segment deletion
SELECT test_slru_page_delete(:pageno_max::BIGINT);
SELECT test_slru_page_exists(:pageno_max::BIGINT);
-- Page truncation
SELECT test_slru_page_exists(:pageno_middle::BIGINT);
SELECT test_slru_page_truncate(:pageno_middle::BIGINT);
SELECT test_slru_page_exists(:pageno_middle::BIGINT);

-- Full deletion
SELECT test_slru_delete_all();
SELECT test_slru_page_exists(:no_such_page);
SELECT test_slru_page_exists(:pageno_middle::BIGINT);
SELECT test_slru_page_exists(:pageno_max::BIGINT);

DROP EXTENSION test_slru;
