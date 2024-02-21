/*--------------------------------------------------------------------------
 *
 * test_dsm.c
 *		Test dynamic shared memory
 *
 * Copyright (c) 2022-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_dsm/test_dsm.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "storage/dsm.h"

PG_MODULE_MAGIC;


/* Test basic DSM functionality */
PG_FUNCTION_INFO_V1(test_dsm_basic);
Datum
test_dsm_basic(PG_FUNCTION_ARGS)
{
	dsm_segment *seg;
	unsigned char *p;
	Size		requested_size;
	Size		created_size;
	Size		attached_size;
	dsm_handle	handle;

	/* Create a small DSM segment */
	requested_size = 1000;
	seg = dsm_create(requested_size, 0);

	handle = dsm_segment_handle(seg);
	created_size = dsm_segment_map_length(seg);
	if (requested_size != created_size)
		elog(ERROR, "DSM size mismatch, requested %lu but created as %lu", requested_size, created_size);

	/* Fill it with data */
	p = dsm_segment_address(seg);
	memset(p, 0x12, requested_size);

	/* Pin the segment so that it's not destroyed when we detach it */
	dsm_pin_segment(seg);

	/* Detach and re-attach it */
	dsm_detach(seg);
	seg = dsm_attach(handle);

	/*
	 * Check the size after re-attaching.  It can be larger than what was
	 * requested originally, because some implementations round it up to the
	 * nearest page size. Tolerate that.
	 */
	attached_size = dsm_segment_map_length(seg);
	if (attached_size < created_size)
		elog(ERROR, "DSM size mismatch, created %lu but attached %lu", created_size, attached_size);
	if (requested_size + 100000 < created_size)
		elog(ERROR, "unexpectdly large size after attach: requested %lu but got %lu", requested_size, created_size);

	/* check contents */
	p = dsm_segment_address(seg);
	for (Size i; i < created_size; i++)
	{
		if (p[i] != 0x12)
			elog(ERROR, "DSM segment has unexpected content %u at offset %lu", p[i], i);
	}

	dsm_detach(seg);

	PG_RETURN_VOID();
}
