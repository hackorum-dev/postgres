/*-------------------------------------------------------------------------
 *
 * subprocess.c
 *
 * Copyright (c) 2004-2020, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/syslogger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "bootstrap/bootstrap.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgwriter.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "postmaster/startup.h"
#include "postmaster/subprocess.h"
#include "postmaster/walwriter.h"
#include "replication/walreceiver.h"

SubprocessType		MySubprocessType;
PgSubprocess	   *MySubprocess;

static PgSubprocess process_types[] = {
	{
		.name = "boot",
		.desc = "checker",
		.needs_aux_proc = true,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = CheckerModeMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "bootstrap",
		.needs_aux_proc = true,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = BootstrapModeMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "startup",
		.needs_aux_proc = true,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = StartupProcessMain,
		.fork_failure = StartupForkFailure
	},
	{
		.name = "boot",
		.desc = "background writer",
		.needs_aux_proc = true,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = BackgroundWriterMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "checkpointer",
		.needs_aux_proc = true,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = CheckpointerMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "wal writer",
		.needs_aux_proc = true,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = WalWriterMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "wal receiver",
		.needs_aux_proc = true,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = WalReceiverMain,
		.fork_failure = NULL
	},
	{
		.name = "avlauncher",
		.desc = "autovacuum launcher",
		.needs_aux_proc = false,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = AutoVacLauncherMain,
		.fork_failure = NULL
	},
	{
		.name = "avworker",
		.desc = "autovacuum worker",
		.needs_aux_proc = false,
		.needs_shmem = true,
		.fork_prep = NULL,
		.entrypoint = AutoVacWorkerMain,
		.fork_failure = NULL
	},
	{
		.name = "col",
		.desc = "statistics collector",
		.needs_aux_proc = false,
		.needs_shmem = false,
		.fork_prep = PgstatCollectorPrep,
		.entrypoint = PgstatCollectorMain,
		.fork_failure = NULL
	},
	{
		.name = "arch",
		.desc = "archiver",
		.needs_aux_proc = false,
		.needs_shmem = false,
		.fork_prep = PgArchiverPrep,
		.entrypoint = PgArchiverMain,
		.fork_failure = NULL
	}
};

void
InitMySubprocess(SubprocessType type)
{
	MySubprocessType = type;
	MySubprocess = &process_types[type];
	MySubprocess->desc = gettext(MySubprocess->desc);
}
