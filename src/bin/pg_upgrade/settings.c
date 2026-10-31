/*
 *	settings.c
 *
 *	checks for stored configuration settings
 *
 *	Copyright (c) 2026, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/settings.c
 */

#include "postgres_fe.h"

#include "access/transam.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"

/*
 * item is a "name=value" string from setconfig or proconfig, or a parameter
 * name from pg_parameter_acl.  object says where it is stored.
 */
typedef struct
{
	char	   *item;
	char	   *object;
} StoredSetting;

typedef struct
{
	StoredSetting *items;
	int			nitems;
	int			maxitems;
} StoredSettingArr;

static StoredSettingArr config_settings;
static StoredSettingArr param_privileges;

static void
add_stored_setting(StoredSettingArr *arr, const char *item, char *object)
{
	if (arr->nitems >= arr->maxitems)
	{
		arr->maxitems = Max(arr->maxitems * 2, 16);
		arr->items = pg_realloc_array(arr->items, StoredSetting, arr->maxitems);
	}
	arr->items[arr->nitems].item = pg_strdup(item);
	arr->items[arr->nitems].object = object;
	arr->nitems++;
}

/* Sort by item, then object, so that each item is probed once */
static int
stored_setting_compare(const void *p1, const void *p2)
{
	const StoredSetting *s1 = (const StoredSetting *) p1;
	const StoredSetting *s2 = (const StoredSetting *) p2;
	int			cmp = strcmp(s1->item, s2->item);

	if (cmp != 0)
		return cmp;
	return strcmp(s1->object, s2->object);
}

/* Collect the function settings of one database */
static void
process_function_settings(DbInfo *dbinfo, PGresult *res, void *arg)
{
	int			ntups = PQntuples(res);
	int			i_proname = PQfnumber(res, "proname");
	int			i_setting = PQfnumber(res, "setting");

	for (int rowno = 0; rowno < ntups; rowno++)
		add_stored_setting(&config_settings,
						   PQgetvalue(res, rowno, i_setting),
						   psprintf(_("function %s in database \"%s\""),
									PQgetvalue(res, rowno, i_proname),
									dbinfo->db_name));
}

/*
 * get_stored_settings()
 *
 *	Collect the role, database and function settings and the parameter
 *	privileges of the old cluster, for check_stored_settings().
 */
void
get_stored_settings(void)
{
	PGconn	   *conn = connectToServer(&old_cluster, "template1");
	PGresult   *res;
	UpgradeTask *task;
	char	   *query;

	/*
	 * Role and database settings.  Skip what is not dumped: ALTER ROLE ALL,
	 * role-wide settings of pg_ roles, and template0.
	 */
	res = executeQueryOrDie(conn,
							"SELECT s.setting, r.rolname, d.datname "
							"FROM (SELECT setrole, setdatabase, "
							"             pg_catalog.unnest(setconfig) AS setting "
							"      FROM pg_catalog.pg_db_role_setting) s "
							"LEFT JOIN pg_catalog.pg_authid r ON r.oid = s.setrole "
							"LEFT JOIN pg_catalog.pg_database d ON d.oid = s.setdatabase "
							"WHERE CASE WHEN s.setdatabase = 0 "
							"           THEN s.setrole <> 0 AND r.rolname !~ '^pg_' "
							"           ELSE d.datname <> 'template0' END");

	for (int rowno = 0; rowno < PQntuples(res); rowno++)
	{
		const char *setting = PQgetvalue(res, rowno, 0);
		const char *rolname = PQgetvalue(res, rowno, 1);
		const char *datname = PQgetvalue(res, rowno, 2);
		char	   *object;

		if (PQgetisnull(res, rowno, 2))
			object = psprintf(_("role \"%s\""), rolname);
		else if (PQgetisnull(res, rowno, 1))
			object = psprintf(_("database \"%s\""), datname);
		else
			object = psprintf(_("role \"%s\" in database \"%s\""),
							  rolname, datname);

		add_stored_setting(&config_settings, setting, object);
	}
	PQclear(res);

	/* Privileges on parameters, new in PostgreSQL 15 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) >= 1500)
	{
		res = executeQueryOrDie(conn,
								"SELECT parname FROM pg_catalog.pg_parameter_acl");

		for (int rowno = 0; rowno < PQntuples(res); rowno++)
			add_stored_setting(&param_privileges,
							   PQgetvalue(res, rowno, 0),
							   psprintf(_("privileges on parameter \"%s\""),
										PQgetvalue(res, rowno, 0)));
		PQclear(res);
	}

	PQfinish(conn);

	/* Function settings, in every database */
	task = upgrade_task_create();
	query = psprintf("SELECT p.oid::pg_catalog.regprocedure AS proname, "
					 "       pg_catalog.unnest(p.proconfig) AS setting "
					 "FROM pg_catalog.pg_proc p "
					 "WHERE p.proconfig IS NOT NULL AND "
					 "      p.oid >= %u",
					 FirstNormalObjectId);
	upgrade_task_add_step(task, query, process_function_settings, true, NULL);
	upgrade_task_run(task, &old_cluster);
	upgrade_task_free(task);
	pfree(query);
}

/*
 * Run command in a savepoint and roll it back.  Return the error message, or
 * NULL on success.
 */
static char *
probe_command(PGconn *conn, const char *command)
{
	PGresult   *res;
	char	   *errmsg = NULL;

	PQclear(executeQueryOrDie(conn, "SAVEPOINT probe"));

	res = PQexec(conn, command);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		const char *msg = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);

		errmsg = pg_strdup(msg ? msg : PQerrorMessage(conn));
	}
	PQclear(res);

	PQclear(executeQueryOrDie(conn, "ROLLBACK TO SAVEPOINT probe"));

	return errmsg;
}

/*
 * Probe each distinct item and report the objects whose item fails.  The
 * report file is opened on first use and returned.
 */
static FILE *
probe_stored_settings(PGconn *conn, StoredSettingArr *arr,
					  void (*build_command) (PGconn *conn, const char *item,
											 PQExpBuffer buf),
					  bool show_item, FILE *script, const char *output_path)
{
	PQExpBuffer cmd = createPQExpBuffer();
	char	   *errmsg = NULL;

	qsort(arr->items, arr->nitems, sizeof(StoredSetting),
		  stored_setting_compare);

	for (int i = 0; i < arr->nitems; i++)
	{
		StoredSetting *s = &arr->items[i];

		/* New item, probe it */
		if (i == 0 || strcmp(s->item, arr->items[i - 1].item) != 0)
		{
			pg_free(errmsg);
			errmsg = NULL;

			resetPQExpBuffer(cmd);
			build_command(conn, s->item, cmd);
			if (cmd->len > 0)
				errmsg = probe_command(conn, cmd->data);
		}

		if (errmsg)
		{
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %m", output_path);
			if (show_item)
				fprintf(script, "%s: %s: %s\n", s->object, s->item, errmsg);
			else
				fprintf(script, "%s: %s\n", s->object, errmsg);
		}
	}

	pg_free(errmsg);
	destroyPQExpBuffer(cmd);

	return script;
}

/*
 * Build the ALTER ROLE ... SET that pg_dump would emit.  Database and
 * function settings are validated the same way, so this covers them too.
 */
static void
build_setting_command(PGconn *conn, const char *item, PQExpBuffer buf)
{
	makeAlterConfigCommand(conn, item, "ROLE", os_info.user, NULL, NULL, buf);
}

/* GRANT SET fails if the new server does not know the parameter */
static void
build_privilege_command(PGconn *conn, const char *item, PQExpBuffer buf)
{
	appendPQExpBuffer(buf, "GRANT SET ON PARAMETER %s TO PUBLIC", fmtId(item));
}

/*
 * check_stored_settings()
 *
 *	Run the restore's commands in the new cluster and roll them back.
 *	set_config() would be too strict, since the restore only warns about
 *	objects that do not exist yet, like a tablespace.
 */
void
check_stored_settings(void)
{
	PGconn	   *conn;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for stored settings the new cluster rejects");

	if (config_settings.nitems == 0 && param_privileges.nitems == 0)
	{
		check_ok();
		return;
	}

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir, "stored_settings.txt");

	conn = connectToServer(&new_cluster, "template1");
	setFmtEncoding(PQclientEncoding(conn));

	/* Hide the notices about objects that are not restored yet */
	PQclear(executeQueryOrDie(conn, "SET client_min_messages = warning"));
	PQclear(executeQueryOrDie(conn, "BEGIN"));

	script = probe_stored_settings(conn, &config_settings,
								   build_setting_command,
								   true, script, output_path);
	script = probe_stored_settings(conn, &param_privileges,
								   build_privilege_command,
								   false, script, output_path);

	PQclear(executeQueryOrDie(conn, "ROLLBACK"));
	PQfinish(conn);

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains roles, databases, functions, or parameter\n"
				 "privileges with stored settings that the new cluster does not accept,\n"
				 "so restoring them would fail.  Remove or change these settings in the\n"
				 "old cluster, for example with ALTER ROLE ... RESET or REVOKE ... ON\n"
				 "PARAMETER, and restart the upgrade.  A list of the problem settings is\n"
				 "in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}
