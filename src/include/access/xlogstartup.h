/*
 * xlogstartup.h
 *
 * PostgreSQL write-ahead log global state variable declarations.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogstartup.h
 */
#ifndef XLOG_STARTUP_H
#define XLOG_STARTUP_H

/*
 * Exported for the functions in timeline.c and xlogarchive.c.  Only valid
 * in the startup process.
 */
extern bool ArchiveRecoveryRequested;
extern bool InArchiveRecovery;
extern bool StandbyMode;
extern char *recoveryRestoreCommand;

#endif							/* XLOG_STARTUP_H */
