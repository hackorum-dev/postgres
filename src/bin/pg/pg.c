/*-------------------------------------------------------------------------
 *
 * pg --- consolidated PostgreSQL command line interface client
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg/pg.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>

#include "common/logging.h"
#include "lib/stringinfo.h"
#include "port.h"

/* internal vars */
static const char *progname;

/* Map user supplied command names to installed executable names. */
typedef struct NameMap {
	const char *cmdname;
	const char *executable;
} NameMap;

/*
 * The standard format is that "name" => "pg_name", but this is not so
 * for some executables, and for compatibility we don't want to change
 * the executable name.  Instead, nonstandard names are listed here.
 */
const NameMap name_map[] = {
	{ .cmdname = "clusterdb", .executable = "clusterdb" },
	{ .cmdname = "createdb", .executable = "createdb" },
	{ .cmdname = "createuser", .executable = "createuser" },
	{ .cmdname = "dropdb", .executable = "dropdb" },
	{ .cmdname = "dropuser", .executable = "dropuser" },
	{ .cmdname = "initdb", .executable = "initdb" },
	{ .cmdname = "bench", .executable = "pgbench" },
	{ .cmdname = "reindexdb", .executable = "reindexdb" },
	{ .cmdname = "vacuumdb", .executable = "vacuumdb" },
	{ .cmdname = NULL }
};

static char *executable_for_command(const char *cmdname);
static void usage(const char *progname);

int
main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IONBF, 0);
	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg"));
	progname = get_progname(argv[0]);

	/*
	 * Most options are handled in the various sub-command executables, not
	 * here.  The only options checked for here are ones that `pg' will handle
	 * even when there is no sub-command to which the arguments will be handed
	 * off.  For now, this is just help and version information.
	 */
	if (argc > 1)
	{
		char	   *found_path;
		char	   *executable;
		char		cmd_version[MAXPGPATH + 512];

		found_path = pg_malloc(MAXPGPATH);

		/* Handle recognized options */
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg (PostgreSQL) " PG_VERSION);
			exit(0);
		}

		/* If it is an unrecognized option, return an appropriate error */
		if (argv[1][0] == '-')
		{
			fprintf(stderr,_("%s: error: invalid option: %s\n"), progname, argv[1]);
			exit(1);
		}

		/* Otherwise, try to interpret the argument as a command name */
		executable = executable_for_command(argv[1]);
		snprintf(cmd_version, sizeof(cmd_version), "%s (PostgreSQL) %s\n", executable, PG_VERSION);
		if (find_other_cmd(argv[0], executable, cmd_version, found_path) == 0)
		{
			StringInfoData		cmd;
			int					argidx;
			int					ret;

			/*
			 * Construct a command string to hand to system() from argv[1..n], noting that
			 * argv[1] is the command name and needs to be rewritten as an absolute path
			 * to the appropriate executable.  For argv[2..n], we do no processing, but
			 * need to be careful to handle any escaping and quoting rules for the system.
			 */
			initStringInfo(&cmd);
			appendStringInfo(&cmd, "%s", found_path);
			for (argidx = 2; argidx < argc; argidx++)
			{
				/* XXX: Is this escaping sufficient? */
				char *arg = escape_single_quotes_ascii(argv[argidx]);
				appendStringInfo(&cmd, " %s", arg);
			}

			ret = system(cmd.data);
			if (ret)
			{
				fprintf(stderr,_("%s: command failed: %s\n"), progname, cmd.data);
				exit(ret >> 8);
			}
			exit(0);
		}

		/* It's either an unrecognized command or garbage.  Charitably assume it is a command */
		fprintf(stderr,_("%s: error: unrecognized command: %s\n"), progname, argv[1]);
		exit(1);
	}

	fprintf(stderr,_("%s: missing command\n"), progname);
	exit(1);
}

static
char *executable_for_command(const char *cmdname)
{
	int i;

	/* Check for nonstandard executable names */
	for (i = 0; name_map[i].cmdname; i++)
	{
		if (strcmp(cmdname, name_map[i].cmdname) == 0)
			return pstrdup(name_map[i].executable);
	}

	/* Otherwise, return standard executable name derived from cmdname */
	return psprintf("pg_%s", cmdname);
}

/*
 * print help text
 */
static void
usage(const char *progname)
{
	printf(_("%s is the consolidated PostgreSQL command line interface client.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [COMMAND] [COMMAND OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nCommands:\n"));
	printf(_("  archivecleanup          remove older WAL files from PostgreSQL archives\n"));
	printf(_("  basebackup              take a base backup of a running PostgreSQL server\n"));
	printf(_("  bench                   run benchmark tests against a PostgreSQL server\n"));
	printf(_("  checksums               enable, disable, or verify data checksums in a PostgreSQL database cluster\n"));
	printf(_("  clusterdb               cluster all previously clustered tables in a database\n"));
	printf(_("  config                  show information about the installed version of PostgreSQL\n"));
	printf(_("  controldata             display control information of a PostgreSQL database cluster\n"));
	printf(_("  createdb                create a PostgreSQL database\n"));
	printf(_("  createuser              create a new PostgreSQL role\n"));
	printf(_("  ctl                     initialize, start, stop, or control a PostgreSQL server\n"));
	printf(_("  dropdb                  remove a PostgreSQL database\n"));
	printf(_("  dropuser                remove a PostgreSQL role\n"));
	printf(_("  dump                    dump a database as a text file or to other formats\n"));
	printf(_("  dumpall                 extract a PostgreSQL database cluster into an SQL script file\n"));
	printf(_("  initdb                  initialize a PostgreSQL database cluster\n"));
	printf(_("  isready                 issue a connection check to a PostgreSQL database\n"));
	printf(_("  receivewal              receive PostgreSQL streaming write-ahead logs\n"));
	printf(_("  recvlogical             control PostgreSQL logical decoding streams\n"));
	printf(_("  reindexdb               reindex a PostgreSQL database\n"));
	printf(_("  resetwal                reset the PostgreSQL write-ahead log\n"));
	printf(_("  restore                 restore a PostgreSQL database from an archive created by pg_dump\n"));
	printf(_("  rewind                  resynchronize a PostgreSQL cluster with another copy of the cluster\n"));
	printf(_("  test_fsync              test all supported fsync() methods\n"));
	printf(_("  test_timing             test overhead of timing calls and their monotonicity\n"));
	printf(_("  upgrade                 upgrade a PostgreSQL cluster to a different major version\n"));
	printf(_("  vacuumdb                clean and analyze a PostgreSQL database\n"));
	printf(_("  verifybackup            verify a backup against the backup manifest\n"));
	printf(_("  waldump                 decode and display PostgreSQL write-ahead logs for debugging\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
