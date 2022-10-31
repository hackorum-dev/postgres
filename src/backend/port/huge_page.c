/*-------------------------------------------------------------------------
 *
 * huge_page.c
 *	  Map .text segment of binary to huge pages
 *
 * TODO: better rationale for separate file if the huge page handling
 * in sysv_shmem.c were moved here.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  src/backend/port/huge_page.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/mman.h>

#include "port/huge_page.h"
#include "storage/fd.h"

/*
 * Collapse specified memory range to huge pages.
 */
static void
CollapseRegionToHugePages(void *addr, size_t advlen)
{
#ifdef __linux__
	size_t advlen_up;
	int r;
	void *r2;
	const size_t bound = 1024*1024*2; // FIXME: x86

	fprintf(stderr, "old advlen: %lx\n", advlen);
	advlen_up = (advlen + bound - 1) & ~(bound - 1);

	/*
	* Increase size of mapping to cover the tailing padding to the next
	* segment. Otherwise all the code in that range can't be put into
	* a huge page (access in the non-mapped range needs to cause a fault,
	* hence can't be in the huge page).
	* XXX: Should proably assert that that space is actually zeroes.
	*/
	r2 = mremap(addr, advlen, advlen_up, 0);
	if (r2 == MAP_FAILED)
		fprintf(stderr, "mremap failed: %m\n");
	else if (r2 != addr)
		fprintf(stderr, "mremap wrong addr: %m\n");
	else
		advlen = advlen_up;

	fprintf(stderr, "new advlen: %lx\n", advlen);

	/*
	* The docs for MADV_COLLAPSE say there should be at least one page
	* in the mapped space "for every eligible hugepage-aligned/sized
	* region to be collapsed". I just forced that. But probably not
	* necessary.
	*/
	r = madvise(addr, advlen, MADV_WILLNEED);
	if (r != 0)
		fprintf(stderr, "MADV_WILLNEED failed: %m\n");

	r = madvise(addr, advlen, MADV_POPULATE_READ);
	if (r != 0)
		fprintf(stderr, "MADV_POPULATE_READ failed: %m\n");

	/*
	* Make huge pages out of it. Requires at least linux 6.1.  We could
	* fall back to MADV_HUGEPAGE if it fails, but it doesn't do all that
	* much in older kernels.
	*/
	r = madvise(addr, advlen, MADV_COLLAPSE);
	if (r != 0)
	{
		fprintf(stderr, "MADV_COLLAPSE failed: %m\n");

		r = madvise(addr, advlen, MADV_HUGEPAGE);
		if (r != 0)
			fprintf(stderr, "MADV_HUGEPAGE failed: %m\n");
	}
#endif
}

/*  Map the postgres .text segment into huge pages. */
void
MapStaticCodeToLargePages(void)
{
#ifdef __linux__
	FILE	   *fp = AllocateFile("/proc/self/maps", "r");
	char		buf[128]; // got this from code reading /proc/meminfo -- enough?
	uintptr_t 	addr;
	uintptr_t 	end;
	void * 		self = &MapStaticCodeToLargePages;

	if (fp)
	{
		while (fgets(buf, sizeof(buf), fp))
		{
			if (sscanf(buf, "%lx-%lx", &addr, &end) == 2 &&
				addr <= (uintptr_t) self && (uintptr_t) self < end)
			{
				fprintf(stderr, "self: %p start: %lx end: %lx\n", self, addr, end);
				CollapseRegionToHugePages((void *) addr, end - addr);
				break;
			}
		}
		FreeFile(fp);
	}
#endif
}
