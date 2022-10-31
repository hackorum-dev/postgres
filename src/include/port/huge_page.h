/*-------------------------------------------------------------------------
 *
 * large_page.h
 *	  Map .text segment of binary to huge pages
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  src/include/port/large_page.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LARGE_PAGE_H
#define LARGE_PAGE_H

extern void MapStaticCodeToLargePages(void);

#endif							/* LARGE_PAGE_H */
