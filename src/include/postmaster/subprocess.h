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
	AutoVacuumLauncherType,
	AutoVacuumWorkerType,
	PgstatCollectorType,
	PgArchiverType,

	NUMSUBPROCESSTYPES			/* Must be last! */
} SubprocessType;

typedef int		(*SubprocessPrep) (int argc, char *argv[]);
typedef void	(*SubprocessEntryPoint) (int argc, char *argv[]);
typedef bool	(*SubprocessForkFailure) (int fork_errno);

/* Current subprocess initializer */
extern void InitMySubprocess(SubprocessType type);

typedef struct PgSubprocess
{
	const char			   *name;
	const char			   *desc;
	bool					needs_aux_proc;
	bool					needs_shmem;
	SubprocessPrep			fork_prep;
	SubprocessEntryPoint	entrypoint;
	SubprocessForkFailure	fork_failure;
} PgSubprocess;

extern SubprocessType				MySubprocessType;
extern PGDLLIMPORT PgSubprocess	   *MySubprocess;

#endif							/* SUBPROCESS_H */
