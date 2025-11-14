CONTROLDATA_LINE("pg_control version number",
				 "%u", ControlFile.pg_control_version)
CONTROLDATA_LINE("Catalog version number",
				 "%u", ControlFile.catalog_version_no)
CONTROLDATA_LINE("Database system identifier",
				 "%" PRIu64, ControlFile.system_identifier)
CONTROLDATA_LINE("Latest checkpoint's TimeLineID",
				 "%u", ControlFile.checkPointCopy.ThisTimeLineID)
CONTROLDATA_LINE("Latest checkpoint's full_page_writes",
				 "%s", (ControlFile.checkPointCopy.fullPageWrites ? _("on") : _("off")))
CONTROLDATA_LINE("Latest checkpoint's NextXID",
				 "%u:%u", EpochFromFullTransactionId(ControlFile.checkPointCopy.nextXid), XidFromFullTransactionId(ControlFile.checkPointCopy.nextXid))
CONTROLDATA_LINE("Latest checkpoint's NextOID",
				 "%u", ControlFile.checkPointCopy.nextOid)
CONTROLDATA_LINE("Latest checkpoint's NextMultiXactId",
				 "%u", ControlFile.checkPointCopy.nextMulti)
CONTROLDATA_LINE("Latest checkpoint's NextMultiOffset",
				 "%" PRIu64, ControlFile.checkPointCopy.nextMultiOffset)
CONTROLDATA_LINE("Latest checkpoint's oldestXID",
				 "%u", ControlFile.checkPointCopy.oldestXid)
CONTROLDATA_LINE("Latest checkpoint's oldestXID's DB",
				 "%u", ControlFile.checkPointCopy.oldestXidDB)
CONTROLDATA_LINE("Latest checkpoint's oldestActiveXID",
				 "%u", ControlFile.checkPointCopy.oldestActiveXid)
CONTROLDATA_LINE("Latest checkpoint's oldestMultiXid",
				 "%u", ControlFile.checkPointCopy.oldestMulti)
CONTROLDATA_LINE("Latest checkpoint's oldestMulti's DB",
				 "%u", ControlFile.checkPointCopy.oldestMultiDB)
CONTROLDATA_LINE("Latest checkpoint's oldestCommitTsXid",
				 "%u", ControlFile.checkPointCopy.oldestCommitTsXid)
CONTROLDATA_LINE("Latest checkpoint's newestCommitTsXid",
				 "%u", ControlFile.checkPointCopy.newestCommitTsXid)
CONTROLDATA_LINE("Maximum data alignment",
				 "%u", ControlFile.maxAlign)
CONTROLDATA_LINE("Database block size",
				 "%u", ControlFile.blcksz)
CONTROLDATA_LINE("Blocks per segment of large relation",
				 "%u", ControlFile.relseg_size)
CONTROLDATA_LINE("Pages per SLRU segment",
				 "%u", ControlFile.slru_pages_per_segment)
CONTROLDATA_LINE("WAL block size",
				 "%u", ControlFile.xlog_blcksz)
CONTROLDATA_LINE("Bytes per WAL segment",
				 "%u", ControlFile.xlog_seg_size)
CONTROLDATA_LINE("Maximum length of identifiers",
				 "%u", ControlFile.nameDataLen)
CONTROLDATA_LINE("Maximum columns in an index",
				 "%u", ControlFile.indexMaxKeys)
CONTROLDATA_LINE("Maximum size of a TOAST chunk",
				 "%u", ControlFile.toast_max_chunk_size)
CONTROLDATA_LINE("Size of a large-object chunk",
				 "%u", ControlFile.loblksize)

/* This is no longer configurable, but users may still expect to see it: */
CONTROLDATA_LINE("Date/time type storage",
				 "%s", _("64-bit integers"))
CONTROLDATA_LINE("Float8 argument passing",
				 "%s", ControlFile.float8ByVal ? _("by value") : _("by reference"))
CONTROLDATA_LINE("Data page checksum version",
				 "%u", ControlFile.data_checksum_version)
CONTROLDATA_LINE("Default char data signedness",
				 "%s", ControlFile.default_char_signedness ? _("signed") : _("unsigned"))
