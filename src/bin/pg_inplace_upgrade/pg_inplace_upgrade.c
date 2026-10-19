/*-------------------------------------------------------------------------
 *
 * pg_inplace_upgrade.c
 *		Drive an in-place major version upgrade of one database cluster
 *
 * An in-place upgrade moves a cluster to a new major version without
 * copying the data directory.  The old version shuts down at an upgrade
 * boundary.  The new version then starts on the same data directory, takes
 * over the cluster, and migrates the catalogs before it admits connections.
 *
 * This program runs on the host of one cluster, like pg_upgrade or
 * pg_createsubscriber.  -D names that cluster's data directory, and -b and
 * -B name the old and new installations.  It reaches other clusters only
 * over libpq.
 *
 * A cluster's "posture" is how far it has come through the upgrade: not
 * prepared, at the boundary, taken over, migrating, migrated, or live.  The
 * posture is derived from the control file and from what earlier commands
 * left in the data directory, the same way the server derives it, together
 * with the cluster's role and which installation owns it.
 *
 * Each command checks that the cluster's posture allows it, and refuses
 * otherwise.  Nothing is kept outside the data directory, and
 * postgresql.conf is not edited.  The settings an upgrade needs go into a
 * separate file, pulled in by an include_if_exists line in
 * postgresql.auto.conf.  The client authentication rules also go into a
 * separate file, pulled in by an include_if_exists line at the top of the
 * cluster's hba file, since hba_file cannot change while the server runs.
 * Both files are withdrawn by renaming them and reloading the server.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_inplace_upgrade/pg_inplace_upgrade.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "common/file_utils.h"
#include "common/inplace_upgrade_posture.h"
#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "upgrade/bundle.h"
#include "utils/pidfile.h"

/*
 * The files this program writes at the top of the data directory.  The
 * configuration file carries the settings forced for the duration of the
 * upgrade; the authentication file admits only replication and the role
 * this program connects as, and rejects everyone else, so that the rules
 * after it in the cluster's hba file do not apply.  Both are withdrawn by
 * renaming them to the disabled name, which stays behind for inspection.
 */
#define PG_AUTOCONF_FILENAME	"postgresql.auto.conf"
#define UPGRADE_CONF_FILE		"pg_inplace_upgrade.conf"
#define UPGRADE_HBA_FILE		"pg_inplace_upgrade_hba.conf"
#define DISABLED_SUFFIX			".disabled"
#define INCLUDE_LINE			"include_if_exists '" UPGRADE_CONF_FILE "'\n"

/* A placeholder setting in the configuration file, to see the reload land */
#define RELOADED_SETTING		"pg_inplace_upgrade.configured"

/* How long to wait for the reload, for client backends, and for a standby */
#define WAIT_SECONDS			60

/*
 * Where the cluster stands, the control file used, and the bin
 * directory of the installation that owns the cluster
 */
typedef struct ClusterPosture
{
	ControlFileData control;
	InplaceUpgradePosture posture;
	const char *bindir;
} ClusterPosture;

static const char *progname;
static char *datadir = NULL;
static char *old_bindir = NULL;
static char *new_bindir = NULL;
static SimpleStringList standbys = {NULL, NULL};
static char *server_log = NULL;

/*
 * The line that pulls the authentication file into the cluster's hba file.
 * An hba file dequotes double quotes only, and a relative name would be
 * taken relative to the hba file, which need not be in the data directory.
 */
static char *hba_include_line = NULL;

static void usage(void);
static int	cmd_check(void);
static int	cmd_prepare(void);
static int	cmd_swap(void);
static int	cmd_status(void);
static const char *posture_name(const ClusterPosture *cp);

static bool file_exists(const char *path);
static char *data_path(const char *name);
static void init_command(PQExpBuffer cmd, const char *bindir,
						 const char *program);
static void append_datadir(PQExpBuffer cmd);
static int	run_command(PQExpBuffer cmd);
static void run_or_fatal(PQExpBuffer cmd, const char *what);
static ClusterPosture *read_posture(void);
pg_noreturn static void refuse(const ClusterPosture *cp, const char *fmt, ...)
			pg_attribute_printf(2, 3);
static const char *db_state_name(DBState state);
static const char *upgrade_flag_name(uint8 flag);
static bool server_running(void);
static int	run_pg_ctl(const char *bindir, const char *options,
					   const char *action);
static int	stop_server(const char *bindir, const char *mode);
static bool cluster_is_standby(void);
static void require_primary(void);

static PGconn *connect_primary(const char *dbname);
static PGconn *connect_standby(const char *conninfo);
static PGresult *query(PGconn *conn, const char *sql);
static bool poll_until(PGconn *conn, const char *sql, const char *expected,
					   const char *what);

static bool refusals(PGconn *conn, bool only_connectability);

static void write_upgrade_files(PGconn *conn);
static void write_file(const char *path, const char *data, size_t len);
static char *read_file(const char *path, size_t *len);
static void replace_file(const char *path, const char *data, size_t len);
static void prepend_line_once(const char *path, const char *line);
static void append_line_once(const char *path, const char *line);
static void append_upgrade_settings(const char *const *settings);
static bool reload_config(void);

static void freeze_catalogs(PGconn *conn);
static bool catalogs_frozen(PGconn *conn);
static bool wait_for_standbys(const char *boundary_lsn);


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{"old-bindir", required_argument, NULL, 'b'},
		{"new-bindir", required_argument, NULL, 'B'},
		{"standby", required_argument, NULL, 'S'},
		{"log", required_argument, NULL, 'l'},
		{NULL, 0, NULL, 0}
	};
	int			c;
	int			option_index;
	const char *command;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_inplace_upgrade"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_inplace_upgrade (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "b:B:D:l:S:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'b':
				old_bindir = pg_strdup(optarg);
				break;
			case 'B':
				new_bindir = pg_strdup(optarg);
				break;
			case 'D':
				datadir = pg_strdup(optarg);
				break;
			case 'l':
				server_log = pg_strdup(optarg);
				break;
			case 'S':
				simple_string_list_append(&standbys, optarg);
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (optind >= argc)
	{
		pg_log_error("no command specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	command = argv[optind++];

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (datadir == NULL)
	{
		pg_log_error("no data directory specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	datadir = make_absolute_path(datadir);
	canonicalize_path(datadir);
	hba_include_line = psprintf("include_if_exists \"%s/%s\"\n",
								datadir, UPGRADE_HBA_FILE);

	if (old_bindir == NULL)
	{
		pg_log_error("no old installation specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	canonicalize_path(old_bindir);

	if (new_bindir == NULL)
	{
		pg_log_error("no new installation specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	canonicalize_path(new_bindir);

	if (strcmp(command, "check") != 0 && strcmp(command, "prepare") != 0 &&
		strcmp(command, "swap") != 0 && strcmp(command, "status") != 0)
	{
		pg_log_error("unrecognized command \"%s\"", command);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (pg_check_dir(datadir) <= 0)
		pg_fatal("data directory \"%s\" does not exist or is not a directory",
				 datadir);
	if (!file_exists(data_path("PG_VERSION")))
		pg_fatal("\"%s\" is not a data directory: no PG_VERSION file", datadir);

	/*
	 * WIP: check that both installations have the programs this tool runs,
	 * that the new one is of this program's major version, and that the old
	 * one is of an earlier one.
	 */

	if (strcmp(command, "check") == 0)
		return cmd_check();
	else if (strcmp(command, "prepare") == 0)
		return cmd_prepare();
	else if (strcmp(command, "swap") == 0)
		return cmd_swap();
	else
		return cmd_status();
}

static void
usage(void)
{
	printf(_("%s upgrades a PostgreSQL database cluster in place.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... COMMAND\n\n"), progname);
	printf(_("Commands:\n"));
	printf(_("  check      check that the running old cluster can be upgraded\n"));
	printf(_("  prepare    refuse client connections, freeze the catalogs, and shut\n"
			 "             the old cluster down at an upgrade boundary\n"));
	printf(_("  swap       start the new version on a cluster shut down at an\n"
			 "             upgrade boundary\n"));
	printf(_("  status     report how far the upgrade of the cluster has got\n"));
	printf(_("\nOptions:\n"));
	printf(_("  -b, --old-bindir=DIR   old cluster executable directory\n"));
	printf(_("  -B, --new-bindir=DIR   new cluster executable directory\n"));
	printf(_("  -D, --pgdata=DATADIR   data directory of the cluster on this host\n"));
	printf(_("  -l, --log=FILENAME     server log file for the new version's start\n"));
	printf(_("  -S, --standby=CONNSTR  connection string of a standby of this cluster\n"
			 "                         (can be given more than once)\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nThe program connects to the cluster over its Unix-domain socket as the\n"
			 "operating system user, who must be a superuser of the cluster.\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

static bool
file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* The path of a file in the data directory; the result is palloc'd */
static char *
data_path(const char *name)
{
	return psprintf("%s/%s", datadir, name);
}

/*
 * Start a command line that runs a program of the installation in bindir.
 * The caller appends the arguments, quoted for the shell.
 */
static void
init_command(PQExpBuffer cmd, const char *bindir, const char *program)
{
	char	   *path = psprintf("%s/%s", bindir, program);

	initPQExpBuffer(cmd);
	appendShellString(cmd, path);
	pfree(path);
}

/* Append the option naming the data directory to a command line */
static void
append_datadir(PQExpBuffer cmd)
{
	appendPQExpBufferStr(cmd, " -D ");
	appendShellString(cmd, datadir);
}

/*
 * Run a command line through the shell, free it, and return the exit
 * status.  The command's output goes where ours does.
 */
static int
run_command(PQExpBuffer cmd)
{
	int			rc;

	pg_log_debug("running: %s", cmd->data);
	fflush(NULL);
	rc = system(cmd->data);
	termPQExpBuffer(cmd);
	return rc;
}

/* Run a command line as run_command() does, and exit on a failure */
static void
run_or_fatal(PQExpBuffer cmd, const char *what)
{
	int			rc = run_command(cmd);

	if (rc != 0)
		pg_fatal("%s: %s", what, wait_result_to_str(rc));
}

/*
 * Read the control file and derive the cluster's posture from it.  The
 * server may be writing the file as we read it, so a read that fails the
 * checksum is retried a few times, as in get_controlfile(). We cannot
 * use that here because it does not read the old version's layout.
 */
static ClusterPosture *
read_posture(void)
{
	ClusterPosture *cp = pg_malloc0(sizeof(ClusterPosture));
	char	   *path = data_path(XLOG_CONTROL_FILE);
	bool		crc_ok = false;

	for (int tries = 0; !crc_ok && tries < 10; tries++)
	{
		int			r;

		if (tries > 0)
			pg_usleep(10000);
		r = InplaceUpgradeReadControlFile(path, &cp->control, &crc_ok);
		if (r < 0)
			pg_fatal("could not read file \"%s\": %m", path);
		if (r != sizeof(ControlFileData))
			pg_fatal("could not read file \"%s\": read %d of %zu",
					 path, r, sizeof(ControlFileData));
	}

	InplaceUpgradeGetPosture(&cp->control, datadir, &cp->posture);
	if (cp->posture.posture == UPGRADE_POSTURE_UNREADABLE)
		pg_fatal("could not tell where the upgrade of the cluster in \"%s\" stands: %s",
				 datadir, cp->posture.detail);
	if (!crc_ok)
		pg_fatal("incorrect checksum in control file \"%s\"", path);

	cp->bindir = (cp->posture.owner == UPGRADE_OWNER_OLD) ? old_bindir : new_bindir;
	pfree(path);
	return cp;
}

/*
 * Refuse to run a command from where the cluster stands: report the error,
 * with the state and the upgrade flag of the control file as its detail,
 * and exit.
 */
static void
refuse(const ClusterPosture *cp, const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);
	pg_log_generic_v(PG_LOG_ERROR, PG_LOG_PRIMARY, _(fmt), ap);
	va_end(ap);
	pg_log_error_detail("The cluster state is \"%s\" and the upgrade flag of the latest checkpoint is \"%s\".",
						db_state_name(cp->control.state),
						upgrade_flag_name(cp->control.checkPointCopy.upgradeFlag));
	exit(1);
}

/*
 * The name of a cluster state, as pg_controldata shows it.  Keep in sync
 * with dbState() in pg_controldata.c.
 */
static const char *
db_state_name(DBState state)
{
	switch (state)
	{
		case DB_STARTUP:
			return _("starting up");
		case DB_SHUTDOWNED:
			return _("shut down");
		case DB_SHUTDOWNED_IN_RECOVERY:
			return _("shut down in recovery");
		case DB_SHUTDOWNING:
			return _("shutting down");
		case DB_IN_CRASH_RECOVERY:
			return _("in crash recovery");
		case DB_IN_ARCHIVE_RECOVERY:
			return _("in archive recovery");
		case DB_IN_PRODUCTION:
			return _("in production");
	}
	return _("unrecognized status code");
}

/*
 * The name of an upgrade flag, as pg_controldata shows it.  Keep in sync
 * with upgrade_flag_str() in pg_controldata.c.
 */
static const char *
upgrade_flag_name(uint8 flag)
{
	switch (flag)
	{
		case UPGRADE_FLAG_NONE:
			return "none";
		case UPGRADE_FLAG_BOUNDARY:
			return "boundary";
		case UPGRADE_FLAG_START:
			return "start";
		case UPGRADE_FLAG_GOLIVE:
			return "go-live";
	}
	return _("unrecognized upgrade flag");
}

static bool
server_running(void)
{
	return file_exists(data_path("postmaster.pid"));
}

/*
 * Run the pg_ctl in bindir on the data directory, with the given options
 * and action, and return its exit status.  A server it starts logs to the
 * log file, if one was given.
 */
static int
run_pg_ctl(const char *bindir, const char *options, const char *action)
{
	PQExpBufferData cmd;

	init_command(&cmd, bindir, "pg_ctl");
	append_datadir(&cmd);
	if (server_log && strcmp(action, "start") == 0)
	{
		appendPQExpBufferStr(&cmd, " -l ");
		appendShellString(&cmd, server_log);
	}
	appendPQExpBufferStr(&cmd, " -s");
	if (options[0] != '\0')
		appendPQExpBuffer(&cmd, " %s", options);
	appendPQExpBuffer(&cmd, " %s", action);
	return run_command(&cmd);
}

/*
 * Stop the server with the pg_ctl in bindir, in the given shutdown mode, and
 * return pg_ctl's exit status
 */
static int
stop_server(const char *bindir, const char *mode)
{
	char		options[32];

	snprintf(options, sizeof(options), "-w -m %s", mode);
	return run_pg_ctl(bindir, options, "stop");
}

static bool
cluster_is_standby(void)
{
	return file_exists(data_path("standby.signal"));
}

static void
require_primary(void)
{
	if (cluster_is_standby())
	{
		pg_log_error("the cluster in \"%s\" is a standby", datadir);
		pg_log_error_hint("Run this command on the primary.");
		exit(1);
	}
}

/*
 * Connect to the cluster in the data directory over its Unix-domain socket,
 * found in postmaster.pid, as the operating system user.
 */
static PGconn *
connect_primary(const char *dbname)
{
	char	   *pidpath = data_path("postmaster.pid");
	FILE	   *fp;
	char		line[MAXPGPATH];
	char	   *port = NULL;
	char	   *sockdir = NULL;
	const char *keywords[5];
	const char *values[5];
	PGconn	   *conn;

	fp = fopen(pidpath, "r");
	if (fp == NULL)
		pg_fatal("could not open file \"%s\": %m", pidpath);
	for (int lineno = 1; lineno <= LOCK_FILE_LINE_SOCKET_DIR; lineno++)
	{
		if (fgets(line, sizeof(line), fp) == NULL)
			pg_fatal("could not read line %d from file \"%s\"", lineno, pidpath);
		(void) pg_strip_crlf(line);
		if (lineno == LOCK_FILE_LINE_PORT)
			port = pg_strdup(line);
		else if (lineno == LOCK_FILE_LINE_SOCKET_DIR)
			sockdir = pg_strdup(line);
	}
	fclose(fp);
	if (sockdir[0] == '\0')
		pg_fatal("the server in \"%s\" listens on no Unix-domain socket", datadir);

	keywords[0] = "host";
	values[0] = sockdir;
	keywords[1] = "port";
	values[1] = port;
	keywords[2] = "dbname";
	values[2] = dbname;
	keywords[3] = "fallback_application_name";
	values[3] = progname;
	keywords[4] = NULL;
	values[4] = NULL;

	conn = PQconnectdbParams(keywords, values, false);
	if (PQstatus(conn) != CONNECTION_OK)
		pg_fatal("could not connect to database \"%s\" of the cluster in \"%s\": %s",
				 dbname, datadir, PQerrorMessage(conn));
	return conn;
}

static PGconn *
connect_standby(const char *conninfo)
{
	PGconn	   *conn = PQconnectdb(conninfo);

	if (PQstatus(conn) != CONNECTION_OK)
		pg_fatal("could not connect to standby \"%s\": %s",
				 conninfo, PQerrorMessage(conn));
	return conn;
}

/* Run a query that must succeed */
static PGresult *
query(PGconn *conn, const char *sql)
{
	PGresult   *res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK &&
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", sql);
		exit(1);
	}
	return res;
}

/*
 * Run a single-value query until it returns the expected value, for at most
 * the timeout.  Returns false on timeout.
 */
static bool
poll_until(PGconn *conn, const char *sql, const char *expected,
		   const char *what)
{
	time_t		deadline = time(NULL) + WAIT_SECONDS;

	for (;;)
	{
		PGresult   *res = query(conn, sql);
		bool		done = (PQntuples(res) == 1 &&
							strcmp(PQgetvalue(res, 0, 0), expected) == 0);

		PQclear(res);
		if (done)
			return true;
		if (time(NULL) >= deadline)
		{
			pg_log_error("timed out after %d seconds waiting for %s",
						 WAIT_SECONDS, what);
			return false;
		}
		pg_usleep(100000);
	}
}

/*
 * The conditions under which the cluster cannot be upgraded.  The migration
 * of the catalogs refuses them as well, once the new version runs; checking
 * them here reports them while the old version still serves.  Returns true
 * if none holds.  With only_connectability, only the databases that admit
 * no connections are checked, which is the one condition that no later step
 * can see past.
 */
static bool
refusals(PGconn *conn, bool only_connectability)
{
	PGresult   *res;
	bool		ok = true;

	/*
	 * 'prepare' connects to each database to freeze its catalogs, so every
	 * database must allow connections.  The exception is template0: initdb
	 * froze it, and it can't change unless it later allowed connections.
	 */
	res = query(conn, "SELECT datname FROM pg_catalog.pg_database "
				"WHERE NOT datallowconn AND datname <> 'template0' ORDER BY 1");
	for (int i = 0; i < PQntuples(res); i++)
	{
		pg_log_error("database \"%s\" does not allow connections",
					 PQgetvalue(res, i, 0));
		ok = false;
	}
	PQclear(res);
	if (only_connectability)
		return ok;

	res = query(conn, "SELECT datname FROM pg_catalog.pg_database "
				"WHERE datconnlimit = -2 ORDER BY 1");
	for (int i = 0; i < PQntuples(res); i++)
	{
		pg_log_error("database \"%s\" is invalid", PQgetvalue(res, i, 0));
		ok = false;
	}
	PQclear(res);

	res = query(conn, "SELECT gid FROM pg_catalog.pg_prepared_xacts ORDER BY 1");
	if (PQntuples(res) > 0)
	{
		pg_log_error(ngettext("cluster has %d prepared transaction",
							  "cluster has %d prepared transactions",
							  PQntuples(res)),
					 PQntuples(res));
		ok = false;
	}
	PQclear(res);

	/*
	 * Temporary slots do not count: they are gone before the old version
	 * shuts down.
	 */
	res = query(conn, "SELECT slot_name FROM pg_catalog.pg_replication_slots "
				"WHERE NOT temporary ORDER BY 1");
	for (int i = 0; i < PQntuples(res); i++)
	{
		pg_log_error("replication slot \"%s\" exists", PQgetvalue(res, i, 0));
		ok = false;
	}
	PQclear(res);

	/*
	 * WIP: the upgrade cannot keep pg_commit_ts yet, so refuse a cluster that
	 * tracks commit timestamps.  Don't turn the setting off here: that
	 * deletes the existing timestamps, which is the user's decision.
	 */
	res = query(conn, "SHOW track_commit_timestamp");
	if (strcmp(PQgetvalue(res, 0, 0), "on") == 0)
	{
		pg_log_error("track_commit_timestamp is on");
		ok = false;
	}
	PQclear(res);

	return ok;
}

static int
cmd_check(void)
{
	PGconn	   *conn;
	PGresult   *res;
	bool		ok;

	require_primary();
	if (!server_running())
		pg_fatal("the cluster in \"%s\" is not running", datadir);

	conn = connect_primary("postgres");
	res = query(conn, "SELECT rolsuper FROM pg_catalog.pg_roles WHERE rolname = current_user");
	if (strcmp(PQgetvalue(res, 0, 0), "t") != 0)
		pg_fatal("role \"%s\" is not a superuser", PQuser(conn));
	PQclear(res);

	ok = refusals(conn, false);
	PQfinish(conn);

	/*
	 * WIP: check that each standby is in recovery and streaming from the
	 * primary, so that it follows it to the upgrade boundary.
	 */

	if (!ok)
	{
		pg_log_error("the cluster in \"%s\" cannot be upgraded", datadir);
		return 1;
	}
	pg_log_info("the cluster in \"%s\" can be upgraded", datadir);
	return 0;
}

/*
 * Write the settings file, pg_inplace_upgrade.conf, and the authentication
 * file, pg_inplace_upgrade_hba.conf, and include them from
 * postgresql.auto.conf and the cluster's hba file.  The authentication file
 * admits the role we connect as over the Unix-domain socket and rejects
 * every other connection.  Its include line goes at the top of the hba
 * file, so the first matching rule is always one of ours.
 */
static void
write_upgrade_files(PGconn *conn)
{
	PQExpBufferData buf;
	PGresult   *res;
	char	   *hba_path;

	initPQExpBuffer(&buf);
	appendPQExpBufferStr(&buf,
						 "# Written by pg_inplace_upgrade: only replication and the role running\n"
						 "# the upgrade are admitted until the upgrade goes live.  Add rules\n"
						 "# before the last two lines to admit anyone else.\n");
	appendPQExpBuffer(&buf, "local\tall\t\"%s\"\tpeer\n", PQuser(conn));

	/*
	 * Physical replication connections match only rules that name the
	 * "replication" keyword, never "all", so the cluster's own replication
	 * rules after the include line still admit the standbys.
	 */
	appendPQExpBufferStr(&buf, "local\tall\tall\treject\n");
	appendPQExpBufferStr(&buf, "host\tall\tall\tall\treject\n");

	write_file(data_path(UPGRADE_HBA_FILE), buf.data, buf.len);

	resetPQExpBuffer(&buf);
	appendPQExpBufferStr(&buf, "# Written by pg_inplace_upgrade\n");
	appendPQExpBufferStr(&buf, RELOADED_SETTING " = on\n");
	appendPQExpBufferStr(&buf, "autovacuum = off\n");
	write_file(data_path(UPGRADE_CONF_FILE), buf.data, buf.len);
	termPQExpBuffer(&buf);

	append_upgrade_settings(NULL);

	res = query(conn, "SHOW hba_file");
	hba_path = pg_strdup(PQgetvalue(res, 0, 0));
	PQclear(res);
	prepend_line_once(hba_path, hba_include_line);
}

/*
 * Read a whole file; the result is palloc'd and NUL-terminated, with its
 * length in *len.
 */
static char *
read_file(const char *path, size_t *len)
{
	FILE	   *fp = fopen(path, "r");
	PQExpBufferData buf;
	char		chunk[4096];
	size_t		n;

	if (fp == NULL)
		pg_fatal("could not open file \"%s\": %m", path);
	initPQExpBuffer(&buf);
	while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
		appendBinaryPQExpBuffer(&buf, chunk, n);
	if (ferror(fp))
		pg_fatal("could not read file \"%s\": %m", path);
	fclose(fp);
	*len = buf.len;
	return buf.data;
}

/*
 * Replace a file's contents through a temporary file and a durable rename,
 * so that no reload sees it half written.
 */
static void
replace_file(const char *path, const char *data, size_t len)
{
	char	   *tmppath = psprintf("%s.pg_inplace_upgrade.tmp", path);

	write_file(tmppath, data, len);
	if (durable_rename(tmppath, path) != 0)
		exit(1);
}

/* Put a line at the top of a file, unless it is there already */
static void
prepend_line_once(const char *path, const char *line)
{
	size_t		len;
	char	   *data = read_file(path, &len);
	size_t		linelen = strlen(line);

	if (len >= linelen && memcmp(data, line, linelen) == 0)
		return;
	data = pg_realloc(data, len + linelen + 1);
	memmove(data + linelen, data, len + 1);
	memcpy(data, line, linelen);
	replace_file(path, data, len + linelen);
	free(data);
}

/* Create a file with the given contents, replacing any file of that name */
static void
write_file(const char *path, const char *data, size_t len)
{
	FILE	   *fp = fopen(path, "w");

	if (fp == NULL)
		pg_fatal("could not create file \"%s\": %m", path);
	if (fwrite(data, len, 1, fp) != 1 || fclose(fp) != 0)
		pg_fatal("could not write file \"%s\": %m", path);
}

/* Append a line to a file unless the file already has it */
static void
append_line_once(const char *path, const char *line)
{
	FILE	   *fp;
	char		buf[MAXPGPATH];

	fp = fopen(path, "r");
	if (fp != NULL)
	{
		while (fgets(buf, sizeof(buf), fp) != NULL)
		{
			if (strcmp(buf, line) == 0)
			{
				fclose(fp);
				return;
			}
		}
		fclose(fp);
	}
	fp = fopen(path, "a");
	if (fp == NULL)
		pg_fatal("could not open file \"%s\": %m", path);
	if (fputs(line, fp) == EOF || fclose(fp) != 0)
		pg_fatal("could not write file \"%s\": %m", path);
}

/*
 * Make sure postgresql.auto.conf includes the configuration file, and add
 * the given settings to it, one per line, if any.  Lines already there are
 * not repeated, so a command run again adds nothing.  The include line is
 * left in place when the file is withdrawn: it names a file that may not
 * exist.
 */
static void
append_upgrade_settings(const char *const *settings)
{
	append_line_once(data_path(PG_AUTOCONF_FILENAME), INCLUDE_LINE);
	for (; settings != NULL && *settings != NULL; settings++)
		append_line_once(data_path(UPGRADE_CONF_FILE), *settings);
}

/* Reload the running server's configuration with the old pg_ctl */
static bool
reload_config(void)
{
	int			rc = run_pg_ctl(old_bindir, "", "reload");

	if (rc != 0)
		pg_log_error("could not reload the server: %s", wait_result_to_str(rc));
	return rc == 0;
}

/*
 * Freeze the catalogs of every database with the new installation's
 * vacuumdb, through the same socket we use.  The VACUUM runs in the old
 * server; the client only picks the SQL.  An error in any database aborts.
 */
static void
freeze_catalogs(PGconn *conn)
{
	PQExpBufferData cmd;

	init_command(&cmd, new_bindir, "vacuumdb");
	appendPQExpBufferStr(&cmd, " --host ");
	appendShellString(&cmd, PQhost(conn));
	appendPQExpBuffer(&cmd, " --port %s --username ", PQport(conn));
	appendShellString(&cmd, PQuser(conn));
	appendPQExpBufferStr(&cmd, " --quiet --all --freeze --schema pg_catalog");
	run_or_fatal(&cmd, "freezing the catalogs failed");
}

/*
 * Check that VACUUM froze every page of every catalog and catalog TOAST
 * table, in each database that allows connections.  VACUUM does not fail
 * when something holds back the transaction horizon.  It just leaves tuples
 * unfrozen, so compare relallfrozen with relpages, which VACUUM has just
 * updated.  The catalog migration rewrites the same relations and raises an
 * error on an unfrozen tuple.
 */
static bool
catalogs_frozen(PGconn *conn)
{
	PGresult   *dbs;
	bool		ok = true;
	char		sql[256];

	snprintf(sql, sizeof(sql),
			 "SELECT relname, relallfrozen, relpages FROM pg_catalog.pg_class "
			 "WHERE oid < %u AND relkind IN ('r', 't') AND relallfrozen <> relpages "
			 "ORDER BY 1", FirstUnpinnedObjectId);

	dbs = query(conn, "SELECT datname FROM pg_catalog.pg_database "
				"WHERE datallowconn AND datconnlimit <> -2 ORDER BY 1");
	for (int i = 0; i < PQntuples(dbs); i++)
	{
		const char *dbname = PQgetvalue(dbs, i, 0);
		PGconn	   *dbconn = connect_primary(dbname);
		PGresult   *res = query(dbconn, sql);

		for (int j = 0; j < PQntuples(res); j++)
		{
			pg_log_error("database \"%s\": %s of %s pages of \"%s\" are frozen",
						 dbname, PQgetvalue(res, j, 1), PQgetvalue(res, j, 2),
						 PQgetvalue(res, j, 0));
			ok = false;
		}
		PQclear(res);
		PQfinish(dbconn);
	}
	PQclear(dbs);
	return ok;
}

/*
 * Wait for each standby to pause at the primary's boundary, which it
 * reports through pg_upgrade_boundary_lsn() once it has replayed it.
 */
static bool
wait_for_standbys(const char *boundary_lsn)
{
	bool		ok = true;

	for (SimpleStringListCell *cell = standbys.head; cell; cell = cell->next)
	{
		PGconn	   *conn = connect_standby(cell->val);
		char	   *what = psprintf("standby \"%s\" to reach the upgrade boundary at %s",
									cell->val, boundary_lsn);

		if (poll_until(conn, "SELECT pg_catalog.pg_upgrade_boundary_lsn()::text",
					   boundary_lsn, what))
			pg_log_info("standby \"%s\" paused at the upgrade boundary", cell->val);
		else
			ok = false;
		PQfinish(conn);
	}
	return ok;
}

static int
cmd_prepare(void)
{
	PGconn	   *conn;
	PGresult   *res;
	ClusterPosture *cp;
	char	   *boundary_lsn;
	int			rc;

	cp = read_posture();
	require_primary();
	if (!server_running())
		pg_fatal("the cluster in \"%s\" is not running", datadir);

	conn = connect_primary("postgres");

	res = query(conn, "SELECT rolsuper FROM pg_catalog.pg_roles WHERE rolname = current_user");
	if (strcmp(PQgetvalue(res, 0, 0), "t") != 0)
		pg_fatal("role \"%s\" is not a superuser", PQuser(conn));
	PQclear(res);
	res = query(conn, "SELECT pg_catalog.pg_is_in_recovery()");
	if (strcmp(PQgetvalue(res, 0, 0), "t") == 0)
		pg_fatal("the cluster in \"%s\" is in recovery", datadir);
	PQclear(res);

	/* Nothing has been changed yet, so refuse before the first change. */
	if (!refusals(conn, true))
		pg_fatal("the cluster in \"%s\" cannot be upgraded", datadir);

	/*
	 * Refuse client connections from here on: put the file admitting only
	 * replication and our role in front of the cluster's own rules, turn
	 * autovacuum off, and terminate the client backends that are connected.
	 * WAL senders keep running, since the standbys need all WAL through the
	 * boundary.  The reload takes effect asynchronously, so wait for a
	 * setting from the configuration file to show it.
	 */
	pg_log_info("refusing client connections");
	write_upgrade_files(conn);
	if (!reload_config())
		exit(1);
	if (!poll_until(conn, "SELECT pg_catalog.current_setting('" RELOADED_SETTING "', true)",
					"on", "the configuration to reload"))
		exit(1);
	res = query(conn, "SELECT pg_catalog.pg_terminate_backend(pid) "
				"FROM pg_catalog.pg_stat_activity "
				"WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()");
	PQclear(res);
	if (!poll_until(conn, "SELECT count(*) FROM pg_catalog.pg_stat_activity "
					"WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()",
					"0", "client backends to terminate"))
		exit(1);

	pg_log_info("freezing the catalogs");
	freeze_catalogs(conn);
	if (!catalogs_frozen(conn))
	{
		pg_log_error("the catalogs could not be frozen in every database");

		/*
		 * WIP: report what holds the transaction horizon back: a standby's
		 * feedback, a replication slot, or the oldest backend_xmin. TODO:
		 * there is a proposal to add a system view for that -- maybe we can
		 * use that here?
		 */
		pg_log_error_hint("End the transactions holding back the horizon, then run \"%s prepare\" again.",
						  progname);
		exit(1);
	}

	/*
	 * Behind the authentication file, the conditions checked here can only
	 * come from our own role; the migration of the catalogs refuses them
	 * again if they appear now.
	 */
	if (!refusals(conn, false))
		pg_fatal("the cluster in \"%s\" cannot be upgraded", datadir);

	/*
	 * Switch WAL segments, so that the boundary segment holds nothing written
	 * before the catalogs were frozen, then request the boundary. From here
	 * on nothing is undone: the shutdown checkpoint is the boundary the
	 * standbys follow.
	 */
	pg_log_info("requesting the upgrade boundary");
	res = query(conn, "SELECT pg_catalog.pg_switch_wal()");
	PQclear(res);
	res = query(conn, "SELECT pg_catalog.pg_request_upgrade_boundary()");
	PQclear(res);

	/* A smart shutdown waits for our own session, so close it first. */
	PQfinish(conn);
	pg_log_info("shutting the old version down");
	rc = stop_server(cp->bindir, "smart");
	if (rc != 0)
		pg_fatal("could not shut the old version down: %s",
				 wait_result_to_str(rc));

	cp = read_posture();
	if (cp->posture.posture != UPGRADE_POSTURE_AT_BOUNDARY)
		refuse(cp, "the cluster in \"%s\" did not shut down at an upgrade boundary",
			   datadir);
	boundary_lsn = psprintf("%X/%08X", LSN_FORMAT_ARGS(cp->control.checkPoint));
	pg_log_info("the cluster is shut down at the upgrade boundary %s",
				boundary_lsn);

	return wait_for_standbys(boundary_lsn) ? 0 : 1;
}

static int
cmd_swap(void)
{
	ClusterPosture *cp;
	int			rc;

	require_primary();
	if (server_running())
		pg_fatal("the cluster in \"%s\" is running", datadir);

	cp = read_posture();
	switch (cp->posture.posture)
	{
		case UPGRADE_POSTURE_AT_BOUNDARY:
			break;
		case UPGRADE_POSTURE_TAKEN_OVER:
		case UPGRADE_POSTURE_MIGRATION_FAILED:
		case UPGRADE_POSTURE_MIGRATED:
			pg_log_error("the cluster in \"%s\" has already been taken over by the new version",
						 datadir);
			pg_log_error_hint("See \"%s status\".", progname);
			exit(1);
		default:
			refuse(cp, "the cluster in \"%s\" is not shut down at an upgrade boundary",
				   datadir);
	}

	/*
	 * Until the upgrade goes live, the WAL summarizer must not read WAL that
	 * a rollback discards, and no commit needs to wait for it.  These go into
	 * the included file rather than onto the command line, so that
	 * withdrawing the file restores the cluster's own settings.
	 */
	{
		static const char *const swap_settings[] = {
			"summarize_wal = off\n",
			"synchronous_commit = off\n",
			NULL
		};

		append_upgrade_settings(swap_settings);
	}

	/*
	 * The new version takes over the cluster as it starts and reports ready
	 * only once the catalogs are migrated, so pg_ctl returns then.
	 */
	pg_log_info("starting the new version");
	rc = run_pg_ctl(new_bindir, "-w", "start");
	if (rc != 0)
	{
		pg_log_error("the new version did not start: %s", wait_result_to_str(rc));
		pg_log_error_hint("See \"%s status\".", progname);
		exit(1);
	}

	pg_log_info("the catalogs are migrated, and the cluster serves superusers only until the upgrade goes live");
	return 0;
}

/*
 * The posture as the 'status' command shows it, with one addition: a
 * primary that is not prepared but has the settings file shows as "prepare
 * in progress", since 'prepare' is either running or stopped partway.
 */
static const char *
posture_name(const ClusterPosture *cp)
{
	switch (cp->posture.posture)
	{
		case UPGRADE_POSTURE_UNREADABLE:
			break;
		case UPGRADE_POSTURE_NOT_PREPARED:
			if (!cluster_is_standby() &&
				file_exists(data_path(UPGRADE_CONF_FILE)))
				return _("prepare in progress");
			return _("not prepared");
		case UPGRADE_POSTURE_AT_BOUNDARY:
			return _("at an upgrade boundary");
		case UPGRADE_POSTURE_TAKEN_OVER:
			return _("taken over");
		case UPGRADE_POSTURE_MIGRATION_FAILED:
			return _("migration failed");
		case UPGRADE_POSTURE_MIGRATED:
			return _("migrated");
		case UPGRADE_POSTURE_LIVE:
			return _("live");
	}
	return _("unreadable");
}

static int
cmd_status(void)
{
	ClusterPosture *cp = read_posture();

	printf(_("Data directory:            %s\n"), datadir);
	printf(_("Role:                      %s\n"),
		   cluster_is_standby() ? "standby" : "primary");
	printf(_("Server:                    %s\n"),
		   server_running() ? "running" : "stopped");
	printf(_("Owner:                     %s\n"),
		   cp->posture.owner == UPGRADE_OWNER_OLD ? "old version" : "new version");
	printf(_("Cluster state:             %s\n"),
		   db_state_name(cp->control.state));
	printf(_("Upgrade posture:           %s\n"), posture_name(cp));
	printf(_("Latest checkpoint:         %X/%08X (upgrade flag %s)\n"),
		   LSN_FORMAT_ARGS(cp->control.checkPoint),
		   upgrade_flag_name(cp->control.checkPointCopy.upgradeFlag));
	printf(_("Upgrade configuration:     %s\n"),
		   file_exists(data_path(UPGRADE_CONF_FILE)) ? "present" :
		   file_exists(data_path(UPGRADE_CONF_FILE DISABLED_SUFFIX)) ? "withdrawn" : "absent");

	if (file_exists(data_path(INPLACE_UPGRADE_MIGRATE_ERROR)))
	{
		char	   *path = data_path(INPLACE_UPGRADE_MIGRATE_ERROR);
		FILE	   *fp = fopen(path, "r");
		char		line[1024];

		if (fp == NULL)
			pg_fatal("could not open file \"%s\": %m", path);
		printf(_("Migration error:\n"));
		while (fgets(line, sizeof(line), fp) != NULL)
			printf("    %s", line);
		fclose(fp);
	}
	return 0;
}
