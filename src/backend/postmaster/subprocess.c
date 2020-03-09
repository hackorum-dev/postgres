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
		.desc = "checker",
		.entrypoint = CheckerModeMain
	},
	{
		.desc = "bootstrap",
		.entrypoint = BootstrapModeMain
	},
	{
		.desc = "startup",
		.entrypoint = StartupProcessMain
	},
	{
		.desc = "background writer",
		.entrypoint = BackgroundWriterMain
	},
	{
		.desc = "checkpointer",
		.entrypoint = CheckpointerMain
	},
	{
		.desc = "wal writer",
		.entrypoint = WalWriterMain
	},
	{
		.desc = "wal receiver",
		.entrypoint = WalReceiverMain
	}
};

void
InitMySubprocess(SubprocessType type)
{
	MySubprocessType = type;
	MySubprocess = &process_types[type];
	MySubprocess->desc = gettext(MySubprocess->desc);
}
