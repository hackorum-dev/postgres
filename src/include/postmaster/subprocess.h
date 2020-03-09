/*-------------------------------------------------------------------------
 *
 * subprocess.h
 *
 * Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * src/include/postmaster/subprocess.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBPROCESS_H
#define SUBPROCESS_H

#include "postmaster.h"

typedef enum
{
	NoProcessType = -1,
	CheckerType = 0,
	BootstrapType,
	StartupType,
	BgWriterType,
	CheckpointerType,
	WalWriterType,
	WalReceiverType,	/* end of Auxiliary Process Forks */

	NUMSUBPROCESSTYPES			/* Must be last! */
} SubprocessType;

typedef void (*SubprocessEntryPoint) (int argc, char *argv[]);

/* Current subprocess initializer */
extern void InitMySubprocess(SubprocessType type);

typedef struct PgSubprocess
{
	const char			   *desc;
	SubprocessEntryPoint	entrypoint;
} PgSubprocess;

extern SubprocessType				MySubprocessType;
extern PGDLLIMPORT PgSubprocess	   *MySubprocess;

#endif							/* SUBPROCESS_H */
