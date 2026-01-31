CONTROLDATA_LINE(CD_CONTROL_VERSION, "pg_control version number",
				 "%u", ControlFile.pg_control_version)
CONTROLDATA_LINE(CD_CATALOG_VERSION, "Catalog version number",
				 "%u", ControlFile.catalog_version_no)
CONTROLDATA_LINE(CD_SYSTEM_IDENTIFIER, "Database system identifier",
				 "%" PRIu64, ControlFile.system_identifier)
CONTROLDATA_LINE(CD_CKPT_TIMELINE, "Latest checkpoint's TimeLineID",
				 "%u", ControlFile.checkPointCopy.ThisTimeLineID)
CONTROLDATA_LINE(CD_CKPT_FPW, "Latest checkpoint's full_page_writes",
				 "%s", (ControlFile.checkPointCopy.fullPageWrites ? _("on") : _("off")))
CONTROLDATA_LINE(CD_CKPT_NEXTXID, "Latest checkpoint's NextXID",
				 "%u:%u", EpochFromFullTransactionId(ControlFile.checkPointCopy.nextXid), XidFromFullTransactionId(ControlFile.checkPointCopy.nextXid))
CONTROLDATA_LINE(CD_CKPT_NEXTOID, "Latest checkpoint's NextOID",
				 "%u", ControlFile.checkPointCopy.nextOid)
CONTROLDATA_LINE(CD_CKPT_NEXTMXID, "Latest checkpoint's NextMultiXactId",
				 "%u", ControlFile.checkPointCopy.nextMulti)
CONTROLDATA_LINE(CD_CKPT_NEXTMXOFF, "Latest checkpoint's NextMultiOffset",
				 "%" PRIu64, ControlFile.checkPointCopy.nextMultiOffset)
CONTROLDATA_LINE(CD_CKPT_OLDESTXID, "Latest checkpoint's oldestXID",
				 "%u", ControlFile.checkPointCopy.oldestXid)
CONTROLDATA_LINE(CD_CKPT_OLDESTXID_DB, "Latest checkpoint's oldestXID's DB",
				 "%u", ControlFile.checkPointCopy.oldestXidDB)
CONTROLDATA_LINE(CD_CKPT_OLDEST_ACTIVEXID, "Latest checkpoint's oldestActiveXID",
				 "%u", ControlFile.checkPointCopy.oldestActiveXid)
CONTROLDATA_LINE(CD_CKPT_OLDEST_MULTI, "Latest checkpoint's oldestMultiXid",
				 "%u", ControlFile.checkPointCopy.oldestMulti)
CONTROLDATA_LINE(CD_CKPT_OLDEST_MULTI_DB, "Latest checkpoint's oldestMulti's DB",
				 "%u", ControlFile.checkPointCopy.oldestMultiDB)
CONTROLDATA_LINE(CD_CKPT_OLDEST_COMMITTS_XID, "Latest checkpoint's oldestCommitTsXid",
				 "%u", ControlFile.checkPointCopy.oldestCommitTsXid)
CONTROLDATA_LINE(CD_CKPT_NEWEST_COMMITTS_XID, "Latest checkpoint's newestCommitTsXid",
				 "%u", ControlFile.checkPointCopy.newestCommitTsXid)
CONTROLDATA_LINE(CD_MAXALIGN, "Maximum data alignment",
				 "%u", ControlFile.maxAlign)
CONTROLDATA_LINE(CD_BLCKSZ, "Database block size",
				 "%u", ControlFile.blcksz)
CONTROLDATA_LINE(CD_RELSEG_SZ, "Blocks per segment of large relation",
				 "%u", ControlFile.relseg_size)
CONTROLDATA_LINE(CD_SLRU_PPS, "Pages per SLRU segment",
				 "%u", ControlFile.slru_pages_per_segment)
CONTROLDATA_LINE(CD_WAL_BLCKSZ, "WAL block size",
				 "%u", ControlFile.xlog_blcksz)
CONTROLDATA_LINE(CD_WAL_SEGSIZE, "Bytes per WAL segment",
				 "%u", ControlFile.xlog_seg_size)
CONTROLDATA_LINE(CD_WAL_NAMEDATALEN, "Maximum length of identifiers",
				 "%u", ControlFile.nameDataLen)
CONTROLDATA_LINE(CD_INDEX_MAX_KEYS, "Maximum columns in an index",
				 "%u", ControlFile.indexMaxKeys)
CONTROLDATA_LINE(CD_TOAST_MAXCHUNKSZ, "Maximum size of a TOAST chunk",
				 "%u", ControlFile.toast_max_chunk_size)
CONTROLDATA_LINE(CD_LO_BLKSZ, "Size of a large-object chunk",
				 "%u", ControlFile.loblksize)

/* This is no longer configurable, but users may still expect to see it: */
CONTROLDATA_LINE(CD_DATETIME_INT64, "Date/time type storage",
				 "%s", _("64-bit integers"))
CONTROLDATA_LINE(CD_FLOAT8_ARGS, "Float8 argument passing",
				 "%s", ControlFile.float8ByVal ? _("by value") : _("by reference"))
CONTROLDATA_LINE(CD_CHECKSUMS, "Data page checksum version",
				 "%u", ControlFile.data_checksum_version)
CONTROLDATA_LINE(CD_CHAR_SIGNEDNESS, "Default char data signedness",
				 "%s", ControlFile.default_char_signedness ? _("signed") : _("unsigned"))
