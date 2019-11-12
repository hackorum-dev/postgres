/*-------------------------------------------------------------------------
 *
 * walwriter.h
 *	  Exports from postmaster/walwriter.c.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * src/include/postmaster/walwriter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALWRITER_H
#define _WALWRITER_H

/* GUC options */
extern int	WalWriterDelay;
extern int	WalWriterFlushAfter;

extern pg_noreturn void WalWriterMain(void);

#endif							/* _WALWRITER_H */
