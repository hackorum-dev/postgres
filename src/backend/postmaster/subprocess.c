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
#include "postmaster/bgwriter.h"
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
		.entrypoint = CheckerModeMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "bootstrap",
		.needs_aux_proc = true,
		.entrypoint = BootstrapModeMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "startup",
		.needs_aux_proc = true,
		.entrypoint = StartupProcessMain,
		.fork_failure = StartupForkFailure
	},
	{
		.name = "boot",
		.desc = "background writer",
		.needs_aux_proc = true,
		.entrypoint = BackgroundWriterMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "checkpointer",
		.needs_aux_proc = true,
		.entrypoint = CheckpointerMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "wal writer",
		.needs_aux_proc = true,
		.entrypoint = WalWriterMain,
		.fork_failure = NULL
	},
	{
		.name = "boot",
		.desc = "wal receiver",
		.needs_aux_proc = true,
		.entrypoint = WalReceiverMain,
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
