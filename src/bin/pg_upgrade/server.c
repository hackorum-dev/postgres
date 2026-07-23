/*
 *	server.c
 *
 *	database server functions
 *
 *	Copyright (c) 2010-2026, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/server.c
 */

#include "postgres_fe.h"

#include "common/connect.h"
#include "fe_utils/string_utils.h"
#include "libpq/pqcomm.h"
#include "pg_upgrade.h"

static PGconn *get_db_conn(ClusterInfo *cluster, const char *db_name);


/*
 * connectToServer()
 *
 *	Connects to the desired database on the designated server.
 *	If the connection attempt fails, this function logs an error
 *	message and calls exit() to kill the program.
 */
PGconn *
connectToServer(ClusterInfo *cluster, const char *db_name)
{
	PGconn	   *conn = get_db_conn(cluster, db_name);

	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(PG_REPORT, "%s", PQerrorMessage(conn));

		if (conn)
			PQfinish(conn);

		printf(_("Failure, exiting\n"));
		exit(1);
	}

	PQclear(executeQueryOrDie(conn, ALWAYS_SECURE_SEARCH_PATH_SQL));

	return conn;
}


/*
 * get_db_conn()
 *
 * get database connection, using named database + standard params for cluster
 *
 * Caller must check for connection failure!
 */
static PGconn *
get_db_conn(ClusterInfo *cluster, const char *db_name)
{
	PQExpBufferData conn_opts;
	PGconn	   *conn;

	/* Build connection string with proper quoting */
	initPQExpBuffer(&conn_opts);
	appendPQExpBufferStr(&conn_opts, "dbname=");
	appendConnStrVal(&conn_opts, db_name);
	appendPQExpBufferStr(&conn_opts, " user=");
	appendConnStrVal(&conn_opts, os_info.user);
	appendPQExpBuffer(&conn_opts, " port=%d", cluster->port);
	if (cluster->sockdir)
	{
		appendPQExpBufferStr(&conn_opts, " host=");
		appendConnStrVal(&conn_opts, cluster->sockdir);
	}
	if (!protocol_negotiation_supported(cluster))
		appendPQExpBufferStr(&conn_opts, " max_protocol_version=3.0");

	conn = PQconnectdb(conn_opts.data);
	termPQExpBuffer(&conn_opts);
	return conn;
}


/*
 * cluster_conn_opts()
 *
 * Return standard command-line options for connecting to this cluster when
 * using psql, pg_dump, etc.  Ideally this would match what get_db_conn()
 * sets, but the utilities we need aren't very consistent about the treatment
 * of database name options, so we leave that out.
 *
 * Result is valid until the next call to this function.
 */
char *
cluster_conn_opts(ClusterInfo *cluster)
{
	static PQExpBuffer buf;

	if (buf == NULL)
		buf = createPQExpBuffer();
	else
		resetPQExpBuffer(buf);

	if (cluster->sockdir)
	{
		appendPQExpBufferStr(buf, "--host ");
		appendShellString(buf, cluster->sockdir);
		appendPQExpBufferChar(buf, ' ');
	}
	appendPQExpBuffer(buf, "--port %d --username ", cluster->port);
	appendShellString(buf, os_info.user);

	return buf->data;
}


/*
 * executeQueryOrDie()
 *
 *	Formats a query string from the given arguments and executes the
 *	resulting query.  If the query fails, this function logs an error
 *	message and calls exit() to kill the program.
 */
PGresult *
executeQueryOrDie(PGconn *conn, const char *fmt, ...)
{
	static char query[QUERY_ALLOC];
	va_list		args;
	PGresult   *result;
	ExecStatusType status;

	va_start(args, fmt);
	vsnprintf(query, sizeof(query), fmt, args);
	va_end(args);

	pg_log(PG_VERBOSE, "executing: %s", query);
	result = PQexec(conn, query);
	status = PQresultStatus(result);

	if ((status != PGRES_TUPLES_OK) && (status != PGRES_COMMAND_OK))
	{
		pg_log(PG_REPORT, "SQL command failed\n%s\n%s", query,
			   PQerrorMessage(conn));
		PQclear(result);
		PQfinish(conn);
		printf(_("Failure, exiting\n"));
		exit(1);
	}
	else
		return result;
}


static void
stop_postmaster_atexit(void)
{
	stop_postmaster(true);
}


/*
 * To start postgres with a particular value for a particular GUC, we can
 * specify -c guc_name=guc_value on the command-line, but we need to
 * shell-escape the string to avoid misbehavior in the case where, for
 * example, guc_value contains spaces or double quotes.
 */
static void
add_pg_config_option(PQExpBuffer postgres_opts,
					 const char *guc_name, const char *guc_value)
{
	char	   *guc_string = psprintf("%s=%s", guc_name, guc_value);

	if (postgres_opts->len > 0)
		appendPQExpBufferChar(postgres_opts, ' ');
	appendPQExpBufferStr(postgres_opts, "-c ");
	appendShellString(postgres_opts, guc_string);
	pfree(guc_string);
}


bool
start_postmaster(ClusterInfo *cluster, bool report_and_exit_on_error)
{
	PQExpBufferData cmd;
	PGconn	   *conn;
	bool		pg_ctl_return = false;
	PQExpBufferData postgres_opts;
	PQExpBufferData socket_opts;

	static bool exit_hook_registered = false;

	if (!exit_hook_registered)
	{
		atexit(stop_postmaster_atexit);
		exit_hook_registered = true;
	}

	/*
	 * Construct options to be passed to the server process.
	 *
	 * Use -b to disable autovacuum and logical replication launcher
	 * (effective in PG17 or later for the latter).
	 */
	initPQExpBuffer(&postgres_opts);
	appendPQExpBuffer(&postgres_opts, "-p %d -b", cluster->port);

	/*
	 * Turn off durability requirements to improve object creation speed, and
	 * we only modify the new cluster, so only use it there.  If there is a
	 * crash, the new cluster has to be recreated anyway.  fsync=off is a big
	 * win on ext4.
	 */
	if (cluster == &new_cluster)
	{
		add_pg_config_option(&postgres_opts, "synchronous_commit", "off");
		add_pg_config_option(&postgres_opts, "fsync", "off");
		add_pg_config_option(&postgres_opts, "full_page_writes", "off");
	}

	initPQExpBuffer(&socket_opts);

#if !defined(WIN32)
	/* prevent TCP/IP connections, restrict socket access */
	add_pg_config_option(&socket_opts, "listen_addresses", "");
	add_pg_config_option(&socket_opts, "unix_socket_permissions", "0700");

	/* Have a sockdir?	Tell the postmaster. */
	if (cluster->sockdir)
		add_pg_config_option(&socket_opts, "unix_socket_directories",
							 cluster->sockdir);
#endif

	/*
	 * Construct the pg_ctl command.
	 *
	 * -o/--old-options or -O/--new-options are documented as allowing the
	 * user to pass through options to the server. To deliver that behavior,
	 * we should shell-escape them before passing them to pg_ctl -o, since we
	 * will use the shell to run pg_ctl. However, the historical behavior of
	 * these flags is actually that they simply wrap the values of the options
	 * in double-quotes, and it's possible that there are users including
	 * shell metacharacters in the values passed to those options and relying
	 * on the faulty escaping for correct operation. Hence, preserve that
	 * behavior for now.
	 *
	 * We do, however, want to escape the other values that we're passing to
	 * pg_ctl -o, so that if, for example, the socket directory contains shell
	 * metacharacters, we nevertheless interpret the value as a literal
	 * pathname. Since appendShellString can only be applied to an entire
	 * option value as a unit, we specify -o three times: once for the options
	 * that precede the user-specified options, once for the user-specified
	 * options, and once for the options that follow the user-specified
	 * options. The order matters, since later options override earlier ones.
	 */
	initPQExpBuffer(&cmd);
	appendPQExpBufferStr(&cmd, quote_shell_path_arg(cluster->bindir, "pg_ctl"));
	appendPQExpBufferStr(&cmd, " -w -l ");
	appendPQExpBufferStr(&cmd,
						 quote_shell_path_arg(log_opts.logdir,
											  SERVER_LOG_FILE));
	appendPQExpBufferStr(&cmd, " -D ");
	appendPQExpBufferStr(&cmd, quote_shell_arg(cluster->pgconfig));

	appendPQExpBufferStr(&cmd, " -o ");
	appendShellString(&cmd, postgres_opts.data);
	if (cluster->pgopts)
		appendPQExpBuffer(&cmd, " -o \"%s\"", cluster->pgopts);
	if (socket_opts.len > 0)
	{
		appendPQExpBufferStr(&cmd, " -o ");
		appendShellString(&cmd, socket_opts.data);
	}
	appendPQExpBufferStr(&cmd, " start");

	termPQExpBuffer(&postgres_opts);
	termPQExpBuffer(&socket_opts);

	/*
	 * Don't throw an error right away, let connecting throw the error because
	 * it might supply a reason for the failure.
	 */
	pg_ctl_return = exec_prog(SERVER_START_LOG_FILE,
	/* pass both file names if they differ */
							  (strcmp(SERVER_LOG_FILE,
									  SERVER_START_LOG_FILE) != 0) ?
							  SERVER_LOG_FILE : NULL,
							  report_and_exit_on_error, false,
							  "%s", cmd.data);

	/* Did it fail and we are just testing if the server could be started? */
	if (!pg_ctl_return && !report_and_exit_on_error)
	{
		termPQExpBuffer(&cmd);
		return false;
	}

	/*
	 * We set this here to make sure atexit() shuts down the server, but only
	 * if we started the server successfully.  We do it before checking for
	 * connectivity in case the server started but there is a connectivity
	 * failure.  If pg_ctl did not return success, we will exit below.
	 *
	 * Pre-9.1 servers do not have PQping(), so we could be leaving the server
	 * running if authentication was misconfigured, so someday we might went
	 * to be more aggressive about doing server shutdowns even if pg_ctl
	 * fails, but now (2013-08-14) it seems prudent to be cautious.  We don't
	 * want to shutdown a server that might have been accidentally started
	 * during the upgrade.
	 */
	if (pg_ctl_return)
		os_info.running_cluster = cluster;

	/*
	 * pg_ctl -w might have failed because the server couldn't be started, or
	 * there might have been a connection problem in _checking_ if the server
	 * has started.  Therefore, even if pg_ctl failed, we continue and test
	 * for connectivity in case we get a connection reason for the failure.
	 */
	if ((conn = get_db_conn(cluster, "template1")) == NULL ||
		PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(PG_REPORT, "\n%s", PQerrorMessage(conn));
		if (conn)
			PQfinish(conn);
		if (cluster == &old_cluster)
			pg_fatal("could not connect to source postmaster started with the command:\n"
					 "%s",
					 cmd.data);
		else
			pg_fatal("could not connect to target postmaster started with the command:\n"
					 "%s",
					 cmd.data);
	}
	PQfinish(conn);
	termPQExpBuffer(&cmd);

	/*
	 * If pg_ctl failed, and the connection didn't fail, and
	 * report_and_exit_on_error is enabled, fail now.  This could happen if
	 * the server was already running.
	 */
	if (!pg_ctl_return)
	{
		if (cluster == &old_cluster)
			pg_fatal("pg_ctl failed to start the source server, or connection failed");
		else
			pg_fatal("pg_ctl failed to start the target server, or connection failed");
	}

	return true;
}


void
stop_postmaster(bool in_atexit)
{
	ClusterInfo *cluster;

	if (os_info.running_cluster == &old_cluster)
		cluster = &old_cluster;
	else if (os_info.running_cluster == &new_cluster)
		cluster = &new_cluster;
	else
		return;					/* no cluster running */

	exec_prog(SERVER_STOP_LOG_FILE, NULL, !in_atexit, !in_atexit,
			  "%s -w -D %s -o \"%s\" %s stop",
			  quote_shell_path_arg(cluster->bindir, "pg_ctl"),
			  quote_shell_arg(cluster->pgconfig),
			  cluster->pgopts ? cluster->pgopts : "",
			  in_atexit ? "-m fast" : "-m smart");

	os_info.running_cluster = NULL;
}


/*
 * check_pghost_envvar()
 *
 * Tests that PGHOST does not point to a non-local server
 */
void
check_pghost_envvar(void)
{
	PQconninfoOption *option;
	PQconninfoOption *start;

	/* Get valid libpq env vars from the PQconndefaults function */

	start = PQconndefaults();

	if (!start)
		pg_fatal("out of memory");

	for (option = start; option->keyword != NULL; option++)
	{
		if (option->envvar && (strcmp(option->envvar, "PGHOST") == 0 ||
							   strcmp(option->envvar, "PGHOSTADDR") == 0))
		{
			const char *value = getenv(option->envvar);

			if (value && strlen(value) > 0 &&
			/* check for 'local' host values */
				(strcmp(value, "localhost") != 0 && strcmp(value, "127.0.0.1") != 0 &&
				 strcmp(value, "::1") != 0 && !is_unixsock_path(value)))
				pg_fatal("libpq environment variable %s has a non-local server value: %s",
						 option->envvar, value);
		}
	}

	/* Free the memory that libpq allocated on our behalf */
	PQconninfoFree(start);
}
