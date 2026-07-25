/*-------------------------------------------------------------------------
 *
 * test_xlogreader_oversized.c
 *	  Demonstrate that XLogReader fails to reject multi-page records whose
 *	  xl_tot_len exceeds XLogRecordMaxSize, and that allocate_recordbuf()
 *	  can overflow uint32 when rounding a near-UINT32_MAX length.
 *
 *	  Crafted multi-page WAL is fed through the frontend XLogReader.  A
 *	  correct reader must reject the record cleanly (return NULL with an
 *	  error).  Unfixed code reallocates with a wrapped size (~40kB) and
 *	  then heap-overflows while reassembling continuation pages.
 *
 *	  Build and run (from a configured tree):
 *	    make -C src/test/modules/test_xlogreader
 *	    ./src/test/modules/test_xlogreader/test_xlogreader_oversized
 *
 *	  Expected with the bug:
 *	    - with asserts: abort on
 *	        Assert(gotlen <= lengthof(save_copy)) in XLogDecodeNextRecord
 *	        (gotlen exceeds 2 pages while total_len still forces reallocation)
 *	    - without asserts: stack/heap overflow around save_copy /
 *	        allocate_recordbuf uint32 wrap
 *	  Expected after fix: exit 0 and a rejection message on stdout.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <string.h>

#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "common/fe_memutils.h"

/* Default segment size; only used for page-header cross-checks. */
#define TEST_WAL_SEGSIZE	(16 * 1024 * 1024)

/*
 * Enough pages for reassembly to reallocate and then overflow a 40kB
 * buffer: first page + a few continuations.  total_len claims ~4GB so
 * the reader keeps consuming pages until it corrupts memory (bug) or
 * rejects (fixed).
 */
#define TEST_NPAGES			16

/* Near UINT32_MAX so allocate_recordbuf() rounding wraps to 0. */
#define TEST_XL_TOT_LEN		((uint32) 0xFFFFF000U)

typedef struct TestWalState
{
	char		pages[TEST_NPAGES][XLOG_BLCKSZ];
	int			npages;
} TestWalState;

static void
init_short_page_header(char *page, XLogRecPtr pageaddr, uint16 info,
					   uint32 rem_len)
{
	XLogPageHeader hdr = (XLogPageHeader) page;

	memset(page, 0, XLOG_BLCKSZ);
	hdr->xlp_magic = XLOG_PAGE_MAGIC;
	hdr->xlp_info = info;
	hdr->xlp_tli = 1;
	hdr->xlp_pageaddr = pageaddr;
	hdr->xlp_rem_len = rem_len;
}

static void
init_long_page_header(char *page, XLogRecPtr pageaddr, uint16 info,
					  uint32 rem_len)
{
	XLogLongPageHeader longhdr = (XLogLongPageHeader) page;

	init_short_page_header(page, pageaddr, info | XLP_LONG_HEADER, rem_len);
	longhdr->xlp_sysid = 0;
	longhdr->xlp_seg_size = TEST_WAL_SEGSIZE;
	longhdr->xlp_xlog_blcksz = XLOG_BLCKSZ;
}

/*
 * Build a multi-page record starting at the first data byte after the long
 * header on page 0.  xl_tot_len is huge; continuation pages advertise
 * matching xlp_rem_len values so the reader enters reassembly.
 */
static XLogRecPtr
build_oversized_multpage_wal(TestWalState *ws)
{
	XLogRecPtr	page0 = 0;
	XLogRecPtr	recptr;
	XLogRecord *rec;
	char	   *payload;
	uint32		gotlen;
	int			i;
	int			phd_long = SizeOfXLogLongPHD;
	int			phd_short = SizeOfXLogShortPHD;

	ws->npages = TEST_NPAGES;

	/* Page 0: long header + start of record */
	init_long_page_header(ws->pages[0], page0, 0, 0);
	recptr = page0 + phd_long;
	payload = ws->pages[0] + phd_long;

	/* Zero-fill usable area, then write the fixed-size record header. */
	memset(payload, 0, XLOG_BLCKSZ - phd_long);
	rec = (XLogRecord *) payload;
	rec->xl_tot_len = TEST_XL_TOT_LEN;
	rec->xl_xid = 0;
	rec->xl_prev = 0;			/* randAccess accepts xl_prev < recptr */
	rec->xl_info = 0;
	rec->xl_rmid = RM_XLOG_ID;
	rec->xl_crc = 0;			/* CRC never reached on the overflow path */

	gotlen = XLOG_BLCKSZ - phd_long;

	/* Continuation pages with consistent xlp_rem_len */
	for (i = 1; i < TEST_NPAGES; i++)
	{
		XLogRecPtr	pageaddr = (XLogRecPtr) i * XLOG_BLCKSZ;
		uint32		rem = TEST_XL_TOT_LEN - gotlen;
		int			usable = XLOG_BLCKSZ - phd_short;

		init_short_page_header(ws->pages[i], pageaddr,
							   XLP_FIRST_IS_CONTRECORD, rem);
		memset(ws->pages[i] + phd_short, 0xAB, usable);
		gotlen += usable;
	}

	return recptr;
}

static int
test_page_read(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr,
			   int reqLen, XLogRecPtr targetRecPtr, char *readBuf)
{
	TestWalState *ws = (TestWalState *) xlogreader->private_data;
	uint64		idx;

	(void) reqLen;
	(void) targetRecPtr;

	if (targetPagePtr % XLOG_BLCKSZ != 0)
		return -1;

	idx = targetPagePtr / XLOG_BLCKSZ;
	if (idx >= (uint64) ws->npages)
		return -1;

	memcpy(readBuf, ws->pages[idx], XLOG_BLCKSZ);
	xlogreader->seg.ws_tli = 1;
	return XLOG_BLCKSZ;
}

static void
test_segment_open(XLogReaderState *xlogreader, XLogSegNo nextSegNo,
				  TimeLineID *tli_p)
{
	(void) xlogreader;
	(void) nextSegNo;
	/* Keep TLI from caller / page_read. */
	if (tli_p && *tli_p == 0)
		*tli_p = 1;
	xlogreader->seg.ws_file = 0;	/* dummy non -1 */
	xlogreader->seg.ws_segno = nextSegNo;
	xlogreader->seg.ws_tli = 1;
}

static void
test_segment_close(XLogReaderState *xlogreader)
{
	xlogreader->seg.ws_file = -1;
}

int
main(int argc, char **argv)
{
	TestWalState ws;
	XLogReaderState *state;
	XLogReaderRoutine routine;
	XLogRecPtr	start;
	XLogRecord *record;
	char	   *errormsg = NULL;

	(void) argc;
	(void) argv;

	memset(&ws, 0, sizeof(ws));
	start = build_oversized_multpage_wal(&ws);

	memset(&routine, 0, sizeof(routine));
	routine.page_read = test_page_read;
	routine.segment_open = test_segment_open;
	routine.segment_close = test_segment_close;

	state = XLogReaderAllocate(TEST_WAL_SEGSIZE, NULL, &routine, &ws);
	if (state == NULL)
	{
		fprintf(stderr, "out of memory allocating XLogReader\n");
		return 2;
	}

	/*
	 * Begin at the crafted record.  A fixed reader must refuse xl_tot_len
	 * above XLogRecordMaxSize (or fail closed on size overflow) without
	 * scribbling past its reassembly buffer.
	 */
	XLogBeginRead(state, start);
	record = XLogReadRecord(state, &errormsg);

	if (record != NULL)
	{
		fprintf(stderr,
				"FAIL: oversized multi-page record was accepted "
				"(xl_tot_len=%u XLogRecordMaxSize=%u)\n",
				TEST_XL_TOT_LEN, (unsigned) XLogRecordMaxSize);
		XLogReaderFree(state);
		return 1;
	}

	if (errormsg == NULL || errormsg[0] == '\0')
	{
		fprintf(stderr,
				"FAIL: reader returned NULL without an error message\n");
		XLogReaderFree(state);
		return 1;
	}

	/* Clean rejection — this is the desired post-fix behavior. */
	printf("OK: rejected oversized record: %s\n", errormsg);
	XLogReaderFree(state);
	return 0;
}
