

--
-- bt_page_items()
--

DROP FUNCTION bt_page_items(text, int8);
CREATE FUNCTION bt_page_items(IN relname text, IN blkno int8,
    IN pretty_print boolean,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT itemlen smallint,
    OUT nulls bool,
    OUT vars bool,
    OUT data text,
    OUT dead boolean,
    OUT htid tid,
    OUT tids tid[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bt_page_items_1_9'
LANGUAGE C STRICT PARALLEL SAFE;


--
-- bt_page_opaque()
--
CREATE FUNCTION bt_page_opaque(IN page bytea,
    OUT lsn pg_lsn,
    OUT btpo_prev INT,
    OUT btpo_next INT,
    OUT btpo_level INT,
    OUT flags text[],
    OUT btpo_cycleid SMALLINT)
AS 'MODULE_PATHNAME', 'bt_page_opaque'
LANGUAGE C STRICT PARALLEL SAFE;
