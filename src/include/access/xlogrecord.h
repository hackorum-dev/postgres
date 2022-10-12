/*
 * xlogrecord.h
 *
 * Definitions for the WAL record format.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecord.h
 */
#ifndef XLOGRECORD_H
#define XLOGRECORD_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "port/pg_crc32c.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * The overall layout of an XLOG record is:
 *		Fixed-size header (XLogRecord struct) + variable-sized header data:
 *		 - xl_cid (0 or 4 bytes)
 *		 - xl_cid (0 or 4 bytes)
 *		 - xl_rmgr_flags (0 or 1 byte)
 *		 - xl_len (0, 1, 2 or 4 bytes)
 *		XLogRecordBlockHeader struct
 *		XLogRecordBlockHeader struct
 *		...
 *		XLogRecordDataHeader[Short|Long] struct
 *		block data
 *		block data
 *		...
 *		main data
 *
 * There can be zero or more XLogRecordBlockHeaders, and 0 or more bytes of
 * rmgr-specific data not associated with a block.  XLogRecord structs
 * always start on MAXALIGN boundaries in the WAL files, but the rest of
 * the fields are not aligned.
 *
 * The XLogRecordBlockHeader, XLogRecordDataHeaderShort and
 * XLogRecordDataHeaderLong structs all begin with a single 'id' byte. It's
 * used to distinguish between block references, and the main data structs.
 *
 * The smallest size that XLogRecord header takes up is now 14 bytes: 8 bytes
 * in xl_prev, 4 in checksum, and 1 in xl_rmid and xl_info each, while the
 * max-sized xlog header now takes up 27 bytes; with 4 bytes each in
 * xl_tot_len, xl_xid and xl_cid, plus one in xl_rminfo.
 */
typedef struct XLogRecord {
	XLogRecPtr	xl_prev;
	pg_crc32c	xl_crc;
	RmgrId		xl_rmid;

	/* Flags for record handling and variable-length header fields */
	uint8		xl_info;
	/*
	 * Without padding:
	 * - depending on flags, length field follows (0, 1, 2 or 4 bytes)
	 * - if HAS_XID, TransactionId follows
	 * - if HAS_CID, CommandID follows
	 * - if HAS_RMINFO, uint8 with rminfo flags follows
	 * - XLogRecordBlockHeaders and XLogRecordDataHeader follow
	 */
} XLogRecord;

/*
 * 
 */
typedef struct XLRHeaderData {
	XLogRecPtr	xl_prev;
	pg_crc32c	xl_crc;
	RmgrId		xl_rmid;
	uint8		xl_info;
	TransactionId xl_xid;
	CommandId	xl_cid;
	uint8		xl_rminfo;
	uint32		xl_tot_len;
} XLRHeaderData;

#define MinXLogHeaderSize	( \
	offsetof(XLogRecord, xl_info) \
	+ sizeof(uint8) /* xl_info */ \
)

#define MaxXLogHeaderSize	( \
	MinXLogHeaderSize \
	+ sizeof(TransactionId) /* xl_xid */ \
	+ sizeof(CommandId) /* xl_cid */ \
	+ sizeof(uint8) /* xl_rminfo */ \
	+ sizeof(uint32) /* xl_len */ \
)

/*
 * Mask for getting the size of the length field
 */
#define XLR2_LEN_MASK			(0x03)

/*
 * IFF the record does not contain any registered data, the length field will
 * be absent (as the size of a plain record is knowable from just the
 * fixed-size struct's flags)
 */
#define XLR2_LEN_ABSENT			0x00
/*
 * Size of the xlog record is <= 255 bytes
 */
#define XLR2_LEN_1B				0x01
/*
 * Size of the xlog record is <= (2^16 - 1)
 */
#define XLR2_LEN_2B				0x02
/*
 * Size of the xlog record is <= (2^32 - 1)
 */
#define XLR2_LEN_4B				0x03

/*
 * Does this record contain an XID? This must be included if the data has
 * transactional visibility.
 */
#define XLR_HAS_XID			0x04

/*
 * Doest this record contain a CID? This must be included if the data has
 * transactional visibility, and remote snapshot transfer support is enabled.
 */
#define XLR_HAS_CID			0x08

/*
 * If the redo manager needs non-zero bits in the header to discern different
 * types of WAL records, this flag is set.
 */
#define XLR_HAS_RMINFO			0x10

/*
 * If a WAL record modifies any relation files, in ways not covered by the
 * usual block references, this flag is set. This is not used for anything
 * by PostgreSQL itself, but it allows external tools that read WAL and keep
 * track of modified blocks to recognize such special record types.
 */
#define XLR_SPECIAL_REL_UPDATE	0x20

/*
 * Enforces consistency checks of replayed WAL at recovery. If enabled,
 * each record will log a full-page write for each block modified by the
 * record and will reuse it afterwards for consistency checks. The caller
 * of XLogInsert can use this value if necessary, but if
 * wal_consistency_checking is enabled for a rmgr this is set unconditionally.
 */
#define XLR_CHECK_CONSISTENCY	0x40

#define XLR_INFO_USERFLAGS		( \
	XLR_HAS_XID \
		| XLR_HAS_CID \
		| XLR_SPECIAL_REL_UPDATE \
		| XLR_CHECK_CONSISTENCY \
)

#define XLRSizeOfHeader(infomask) ( \
	MinXLogHeaderSize \
		+ ((1 << ((infomask) & XLR2_LEN_MASK)) >> 1) \
		+ (((infomask) & XLR_HAS_XID) ? sizeof(TransactionId) : 0) \
		+ (((infomask) & XLR_HAS_CID) ? sizeof(CommandId) : 0) \
		+ (((infomask) & XLR_HAS_RMINFO) ? sizeof(uint8) : 0) \
)

static inline uint32
XLogRecordGetLength(XLogRecord *record)
{
	char *lenptr = (char *) record;
	uint8 len8;
	uint16 len16;
	uint32 len32;

	lenptr += MinXLogHeaderSize;

	switch ((record->xl_info) & XLR2_LEN_MASK) {
		case XLR2_LEN_ABSENT:
			return XLRSizeOfHeader(record->xl_info);
		case XLR2_LEN_1B:
			memcpy(&len8, lenptr, sizeof(uint8));
			return (uint32) len8;
		case XLR2_LEN_2B:
			memcpy(&len16, lenptr, sizeof(uint16));
			return (uint32) len16;
		case XLR2_LEN_4B:
			memcpy(&len32, lenptr, sizeof(uint32));
			return (uint32) len32;
		default:
			pg_unreachable();
	}
}

static inline int8
XLogRecordGetRMInfo(XLogRecord *record)
{
	int infooff = MinXLogHeaderSize;

	if (!(record->xl_info & XLR_HAS_RMINFO))
		return 0;

	infooff += (1 << (record->xl_info & XLR2_LEN_MASK)) >> 1;

	if (record->xl_info & XLR_HAS_XID)
		infooff += sizeof(TransactionId);

	if (record->xl_info & XLR_HAS_CID)
		infooff += sizeof(CommandId);

	return *(((uint8 *) record) + infooff);
}

/*
 * Header info for block data appended to an XLOG record.
 *
 * 'data_length' is the length of the rmgr-specific payload data associated
 * with this block. It does not include the possible full page image, nor
 * XLogRecordBlockHeader struct itself.
 *
 * Note that we don't attempt to align the XLogRecordBlockHeader struct!
 * So, the struct must be copied to aligned local storage before use.
 * 
 * Note that if .id & 0x80 is set; the block header is small instead.
 * If .id & 0x40 is set, there will be id + 1 following block headers
 * of this type, having incremental ids, but written to disk without
 * the id field.
 */
typedef struct XLogRecordBlockHeader
{
	uint8		id;				/* block reference ID */
	uint8		fork_flags;		/* fork within the relation, and flags */
	uint16		data_length;	/* number of payload bytes (not including page
								 * image). Emitted iff BKPBLOCK_HAS_DATA */

	/* If BKPBLOCK_HAS_IMAGE, an XLogRecordBlockImageHeader struct follows */
	/* If BKPBLOCK_SAME_REL is not set, a RelFileLocator follows */
	/* BlockNumber follows */
} XLogRecordBlockHeader;

#define SizeOfXLogRecordBlockHeader (offsetof(XLogRecordBlockHeader, data_length) + sizeof(uint16))

/*
 * Additional header information when a full-page image is included
 * (i.e. when BKPBLOCK_HAS_IMAGE is set).
 *
 * The XLOG code is aware that PG data pages usually contain an unused "hole"
 * in the middle, which contains only zero bytes.  Since we know that the
 * "hole" is all zeros, we remove it from the stored data (and it's not counted
 * in the XLOG record's CRC, either).  Hence, the amount of block data actually
 * present is (BLCKSZ - <length of "hole" bytes>).
 *
 * Additionally, when wal_compression is enabled, we will try to compress full
 * page images using one of the supported algorithms, after removing the
 * "hole". This can reduce the WAL volume, but at some extra cost of CPU spent
 * on the compression during WAL logging. In this case, since the "hole"
 * length cannot be calculated by subtracting the number of page image bytes
 * from BLCKSZ, basically it needs to be stored as an extra information.
 * But when no "hole" exists, we can assume that the "hole" length is zero
 * and no such an extra information needs to be stored. Note that
 * the original version of page image is stored in WAL instead of the
 * compressed one if the number of bytes saved by compression is less than
 * the length of extra information. Hence, when a page image is successfully
 * compressed, the amount of block data actually present is less than
 * BLCKSZ - the length of "hole" bytes - the length of extra information.
 */
typedef struct XLogRecordBlockImageHeader
{
	uint16		length;			/* number of page image bytes */
	uint16		hole_offset;	/* number of bytes before "hole" */
	uint8		bimg_info;		/* flag bits, see below */

	/*
	 * If BKPIMAGE_HAS_HOLE and BKPIMAGE_COMPRESSED(), an
	 * XLogRecordBlockCompressHeader struct follows.
	 */
} XLogRecordBlockImageHeader;

#define SizeOfXLogRecordBlockImageHeader	\
	(offsetof(XLogRecordBlockImageHeader, bimg_info) + sizeof(uint8))

/* Information stored in bimg_info */
#define BKPIMAGE_HAS_HOLE		0x01	/* page image has "hole" */
#define BKPIMAGE_APPLY			0x02	/* page image should be restored
										 * during replay */
/* compression methods supported */
#define BKPIMAGE_COMPRESS_PGLZ	0x04
#define BKPIMAGE_COMPRESS_LZ4	0x08
#define BKPIMAGE_COMPRESS_ZSTD	0x10

#define	BKPIMAGE_COMPRESSED(info) \
	(((info) & (BKPIMAGE_COMPRESS_PGLZ | BKPIMAGE_COMPRESS_LZ4 | \
			  BKPIMAGE_COMPRESS_ZSTD)) != 0)

/*
 * Extra header information used when page image has "hole" and
 * is compressed.
 */
typedef struct XLogRecordBlockCompressHeader
{
	uint16		hole_length;	/* number of bytes in "hole" */
} XLogRecordBlockCompressHeader;

#define SizeOfXLogRecordBlockCompressHeader \
	sizeof(XLogRecordBlockCompressHeader)

/*
 * Maximum size of the header for a block reference. This is used to size a
 * temporary buffer for constructing the header.
 */
#define MaxSizeOfXLogRecordBlockHeader \
	(SizeOfXLogRecordBlockHeader + \
	 SizeOfXLogRecordBlockImageHeader + \
	 SizeOfXLogRecordBlockCompressHeader + \
	 sizeof(RelFileLocator) + \
	 sizeof(BlockNumber))

/*
 * The fork number fits in the lower 4 bits in the fork_flags field. The upper
 * bits are used for flags.
 */
#define BKPBLOCK_FORK_MASK	0x0F
#define BKPBLOCK_FLAG_MASK	0xF0
#define BKPBLOCK_HAS_IMAGE	0x10	/* block data is an XLogRecordBlockImage */
#define BKPBLOCK_HAS_DATA	0x20
#define BKPBLOCK_WILL_INIT	0x40	/* redo will re-init the page */
#define BKPBLOCK_SAME_REL	0x80	/* RelFileLocator omitted, same as
									 * previous */

/*
 * XLogRecordDataHeaderShort/Long are used for the "main data" portion of
 * the record. If the length of the data is less than 256 bytes, the short
 * form is used, with a single byte to hold the length. Otherwise the long
 * form is used.
 *
 * (These structs are currently not used in the code, they are here just for
 * documentation purposes).
 */
typedef struct XLogRecordDataHeaderShort
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_SHORT */
	uint8		data_length;	/* number of payload bytes */
}			XLogRecordDataHeaderShort;

#define SizeOfXLogRecordDataHeaderShort (sizeof(uint8) * 2)

typedef struct XLogRecordDataHeaderLong
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_LONG */
	/* followed by uint32 data_length, unaligned */
}			XLogRecordDataHeaderLong;

#define SizeOfXLogRecordDataHeaderLong (sizeof(uint8) + sizeof(uint32))

/*
 * Block IDs used to distinguish different kinds of record fragments. Block
 * references are numbered from 0 to XLR_MAX_BLOCK_ID. A rmgr is free to use
 * any ID number in that range (although you should stick to small numbers,
 * because the WAL machinery is optimized for that case). A few ID
 * numbers are reserved to denote the "main" data portion of the record,
 * as well as replication-supporting transaction metadata.
 *
 * The maximum is currently set at 32, quite arbitrarily. Most records only
 * need a handful of block references, but there are a few exceptions that
 * need more.
 */
#define XLR_MAX_BLOCK_ID			0x20

#define XLR_BLOCK_ID_DATA_SHORT		0x3F
#define XLR_BLOCK_ID_DATA_LONG		0x3E
#define XLR_BLOCK_ID_ORIGIN			0x3D
#define XLR_BLOCK_ID_TOPLEVEL_XID	0x3C

#define XLR_BLOCK_ID_MASK			0x3F
#define XLR_BLOCK_FIRST_NP1_SEQ		0x40 /* the following blocks are 
										  * 0..block_id + 1, and have omitted
										  * their block ID */
#define XLR_BLOCK_DATA_SMALL		0x80 /* data_length field is uint8 */

#endif							/* XLOGRECORD_H */
