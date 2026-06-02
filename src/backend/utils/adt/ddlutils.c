/*-------------------------------------------------------------------------
 *
 * ddlutils.c
 *		Utility functions for generating DDL statements
 *
 * This file contains the pg_get_*_ddl family of functions that generate
 * DDL statements to recreate database objects such as roles, tablespaces,
 * and databases, along with common infrastructure for option parsing and
 * pretty-printing.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ddlutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/toast_compression.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/dependency.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_am.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/partition.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "rewrite/prs2lock.h"
#include "nodes/nodes.h"
#include "commands/tablespace.h"
#include "common/relpath.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/* Option value types for DDL option parsing */
typedef enum
{
	DDL_OPT_BOOL,
	DDL_OPT_TEXT,
	DDL_OPT_INT,
} DdlOptType;

/*
 * A single DDL option descriptor: caller fills in name and type,
 * parse_ddl_options fills in isset + the appropriate value field.
 */
typedef struct DdlOption
{
	const char *name;			/* option name (case-insensitive match) */
	DdlOptType	type;			/* expected value type */
	bool		isset;			/* true if caller supplied this option */
	/* fields for specific option types */
	union
	{
		bool		boolval;	/* filled in for DDL_OPT_BOOL */
		char	   *textval;	/* filled in for DDL_OPT_TEXT (palloc'd) */
		int			intval;		/* filled in for DDL_OPT_INT */
	};
} DdlOption;


static void parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start,
							  DdlOption *opts, int nopts);
static void append_ddl_option(StringInfo buf, bool pretty, int indent,
							  const char *fmt, ...)
			pg_attribute_printf(4, 5);
static void append_guc_value(StringInfo buf, const char *name,
							 const char *value);
static List *pg_get_role_ddl_internal(Oid roleid, bool pretty,
									  bool memberships);
static List *pg_get_tablespace_ddl_internal(Oid tsid, bool pretty, bool no_owner);
static Datum pg_get_tablespace_ddl_srf(FunctionCallInfo fcinfo, Oid tsid, bool isnull);
static List *pg_get_database_ddl_internal(Oid dbid, bool pretty,
										  bool no_owner, bool no_tablespace);
static List *pg_get_table_ddl_internal(Oid relid, bool pretty,
									   bool no_owner, bool no_tablespace,
									   bool include_indexes,
									   bool include_constraints,
									   bool include_rules,
									   bool include_statistics,
									   bool include_triggers,
									   bool include_policies,
									   bool include_rls,
									   bool include_replica_identity,
									   bool include_partition);
static void append_column_defs(StringInfo buf, Relation rel, bool pretty,
							   bool include_constraints);
static void append_typed_column_overrides(StringInfo buf, Relation rel,
										  bool pretty, bool include_constraints);
static void append_inline_check_constraints(StringInfo buf, Relation rel,
											bool pretty, bool *first);
static char *find_attrdef_text(Relation rel, AttrNumber attnum,
							   List **dpcontext);
static char *lookup_qualified_relname(Oid relid);
static List *get_inheritance_parents(Oid relid);


/*
 * parse_ddl_options
 * 		Parse variadic name/value option pairs
 *
 * Options are passed as alternating key/value text pairs.  The caller
 * provides an array of DdlOption descriptors specifying the accepted
 * option names and their types; this function matches each supplied
 * pair against the array, validates the value, and fills in the
 * result fields.
 */
static void
parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start,
				  DdlOption *opts, int nopts)
{
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;
	int			nargs;

	/* Clear all output fields */
	for (int i = 0; i < nopts; i++)
	{
		opts[i].isset = false;
		switch (opts[i].type)
		{
			case DDL_OPT_BOOL:
				opts[i].boolval = false;
				break;
			case DDL_OPT_TEXT:
				opts[i].textval = NULL;
				break;
			case DDL_OPT_INT:
				opts[i].intval = 0;
				break;
		}
	}

	nargs = extract_variadic_args(fcinfo, variadic_start, true,
								  &args, &types, &nulls);

	if (nargs <= 0)
		return;

	/* Handle DEFAULT NULL case */
	if (nargs == 1 && nulls[0])
		return;

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("variadic arguments must be name/value pairs"),
				 errhint("Provide an even number of variadic arguments that can be divided into pairs.")));

	/*
	 * For each option name/value pair, find corresponding positional option
	 * for the option name, and assign the option value.
	 */
	for (int i = 0; i < nargs; i += 2)
	{
		char	   *name;
		char	   *valstr;
		DdlOption  *opt = NULL;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option name at variadic position %d is null", i + 1)));

		name = TextDatumGetCString(args[i]);

		if (nulls[i + 1])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("value for option \"%s\" must not be null", name)));

		/* Find matching option descriptor */
		for (int j = 0; j < nopts; j++)
		{
			if (pg_strcasecmp(name, opts[j].name) == 0)
			{
				opt = &opts[j];
				break;
			}
		}

		if (opt == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized option: \"%s\"", name)));

		if (opt->isset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" is specified more than once",
							name)));

		valstr = TextDatumGetCString(args[i + 1]);

		switch (opt->type)
		{
			case DDL_OPT_BOOL:
				if (!parse_bool(valstr, &opt->boolval))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for boolean option \"%s\": %s",
									name, valstr)));
				break;

			case DDL_OPT_TEXT:
				opt->textval = valstr;
				valstr = NULL;	/* don't pfree below */
				break;

			case DDL_OPT_INT:
				{
					char	   *endp;
					long		val;

					errno = 0;
					val = strtol(valstr, &endp, 10);
					if (*endp != '\0' || errno == ERANGE ||
						val < PG_INT32_MIN || val > PG_INT32_MAX)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for integer option \"%s\": %s",
										name, valstr)));
					opt->intval = (int) val;
				}
				break;
		}

		opt->isset = true;

		if (valstr)
			pfree(valstr);
		pfree(name);
	}
}

/*
 * Helper to append a formatted string with optional pretty-printing.
 */
static void
append_ddl_option(StringInfo buf, bool pretty, int indent,
				  const char *fmt, ...)
{
	if (pretty)
	{
		appendStringInfoChar(buf, '\n');
		appendStringInfoSpaces(buf, indent);
	}
	else
		appendStringInfoChar(buf, ' ');

	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(buf, needed);
	}
}

/*
 * append_guc_value
 *		Append a GUC setting value to buf, handling GUC_LIST_QUOTE properly.
 *
 * Variables marked GUC_LIST_QUOTE were already fully quoted before they
 * were stored in the setconfig array.  We break the list value apart
 * and re-quote the elements as string literals.  For all other variables
 * we simply quote the value as a single string literal.
 *
 * The caller has already appended "SET <name> TO " to buf.
 */
static void
append_guc_value(StringInfo buf, const char *name, const char *value)
{
	char	   *rawval;

	rawval = pstrdup(value);

	if (GetConfigOptionFlags(name, true) & GUC_LIST_QUOTE)
	{
		List	   *namelist;
		bool		first = true;

		/* Parse string into list of identifiers */
		if (!SplitGUCList(rawval, ',', &namelist))
		{
			/* this shouldn't fail really */
			elog(ERROR, "invalid list syntax in setconfig item");
		}
		/* Special case: represent an empty list as NULL */
		if (namelist == NIL)
			appendStringInfoString(buf, "NULL");
		foreach_ptr(char, curname, namelist)
		{
			if (first)
				first = false;
			else
				appendStringInfoString(buf, ", ");
			appendStringInfoString(buf, quote_literal_cstr(curname));
		}
		list_free(namelist);
	}
	else
		appendStringInfoString(buf, quote_literal_cstr(rawval));

	pfree(rawval);
}

/*
 * pg_get_role_ddl_internal
 *		Generate DDL statements to recreate a role
 *
 * Returns a List of palloc'd strings, each being a complete SQL statement.
 * The first list element is always the CREATE ROLE statement; subsequent
 * elements are ALTER ROLE SET statements for any role-specific or
 * role-in-database configuration settings.  If memberships is true,
 * GRANT statements for role memberships are appended.
 */
static List *
pg_get_role_ddl_internal(Oid roleid, bool pretty, bool memberships)
{
	HeapTuple	tuple;
	Form_pg_authid roleform;
	StringInfoData buf;
	char	   *rolname;
	Datum		rolevaliduntil;
	bool		isnull;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	List	   *statements = NIL;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role with OID %u does not exist", roleid)));

	roleform = (Form_pg_authid) GETSTRUCT(tuple);
	rolname = pstrdup(NameStr(roleform->rolname));

	/* User must have SELECT privilege on pg_authid. */
	if (pg_class_aclcheck(AuthIdRelationId, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for role %s", rolname)));
	}

	/*
	 * We don't support generating DDL for system roles.  The primary reason
	 * for this is that users shouldn't be recreating them.
	 */
	if (IsReservedName(rolname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved", rolname),
				 errdetail("Role names starting with \"pg_\" are reserved for system roles.")));

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE ROLE %s", quote_identifier(rolname));

	/*
	 * Append role attributes.  The order here follows the same sequence as
	 * you'd typically write them in a CREATE ROLE command, though any order
	 * is actually acceptable to the parser.
	 */
	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolsuper ? "SUPERUSER" : "NOSUPERUSER");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolinherit ? "INHERIT" : "NOINHERIT");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcreaterole ? "CREATEROLE" : "NOCREATEROLE");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcreatedb ? "CREATEDB" : "NOCREATEDB");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcanlogin ? "LOGIN" : "NOLOGIN");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolreplication ? "REPLICATION" : "NOREPLICATION");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolbypassrls ? "BYPASSRLS" : "NOBYPASSRLS");

	/*
	 * CONNECTION LIMIT is only interesting if it's not -1 (the default,
	 * meaning no limit).
	 */
	if (roleform->rolconnlimit >= 0)
		append_ddl_option(&buf, pretty, 4, "CONNECTION LIMIT %d",
						  roleform->rolconnlimit);

	rolevaliduntil = SysCacheGetAttr(AUTHOID, tuple,
									 Anum_pg_authid_rolvaliduntil,
									 &isnull);
	if (!isnull)
	{
		TimestampTz ts;
		int			tz;
		struct pg_tm tm;
		fsec_t		fsec;
		const char *tzn;
		char		ts_str[MAXDATELEN + 1];

		ts = DatumGetTimestampTz(rolevaliduntil);
		if (TIMESTAMP_NOT_FINITE(ts))
			EncodeSpecialTimestamp(ts, ts_str);
		else if (timestamp2tm(ts, &tz, &tm, &fsec, &tzn, NULL) == 0)
			EncodeDateTime(&tm, fsec, true, tz, tzn, USE_ISO_DATES, ts_str);
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		append_ddl_option(&buf, pretty, 4, "VALID UNTIL %s",
						  quote_literal_cstr(ts_str));
	}

	ReleaseSysCache(tuple);

	/*
	 * We intentionally omit PASSWORD.  There's no way to retrieve the
	 * original password text from the stored hash, and even if we could,
	 * exposing passwords through a SQL function would be a security issue.
	 * Users must set passwords separately after recreating roles.
	 */

	appendStringInfoChar(&buf, ';');

	statements = lappend(statements, pstrdup(buf.data));

	/*
	 * Now scan pg_db_role_setting for ALTER ROLE SET configurations.
	 *
	 * These can be role-wide (setdatabase = 0) or specific to a particular
	 * database (setdatabase = a valid DB OID).  It generates one ALTER
	 * statement per setting.
	 */
	rel = table_open(DbRoleSettingRelationId, AccessShareLock);
	ScanKeyInit(&scankey,
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(roleid));
	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 1, &scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_db_role_setting setting = (Form_pg_db_role_setting) GETSTRUCT(tuple);
		Oid			datid = setting->setdatabase;
		Datum		datum;
		ArrayType  *role_settings;
		Datum	   *settings;
		bool	   *nulls;
		int			nsettings;
		char	   *datname = NULL;

		/*
		 * If setdatabase is valid, this is a role-in-database setting;
		 * otherwise it's a role-wide setting.  Look up the database name once
		 * for all settings in this row.
		 */
		if (OidIsValid(datid))
		{
			datname = get_database_name(datid);
			/* Database has been dropped; skip all settings in this row. */
			if (datname == NULL)
				continue;
		}

		/*
		 * The setconfig column is a text array in "name=value" format. It
		 * should never be null for a valid row, but be defensive.
		 */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		if (isnull)
			continue;

		role_settings = DatumGetArrayTypePCopy(datum);

		deconstruct_array_builtin(role_settings, TEXTOID, &settings, &nulls, &nsettings);

		for (int i = 0; i < nsettings; i++)
		{
			char	   *s,
					   *p;

			if (nulls[i])
				continue;

			s = TextDatumGetCString(settings[i]);
			p = strchr(s, '=');
			if (p == NULL)
			{
				pfree(s);
				continue;
			}
			*p++ = '\0';

			/* Build a fresh ALTER ROLE statement for this setting */
			resetStringInfo(&buf);
			appendStringInfo(&buf, "ALTER ROLE %s", quote_identifier(rolname));

			if (datname != NULL)
				appendStringInfo(&buf, " IN DATABASE %s",
								 quote_identifier(datname));

			appendStringInfo(&buf, " SET %s TO ",
							 quote_identifier(s));

			append_guc_value(&buf, s, p);

			appendStringInfoChar(&buf, ';');

			statements = lappend(statements, pstrdup(buf.data));

			pfree(s);
		}

		pfree(settings);
		pfree(nulls);
		pfree(role_settings);

		if (datname != NULL)
			pfree(datname);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * Scan pg_auth_members for role memberships.  We look for rows where
	 * member = roleid, meaning this role has been granted membership in other
	 * roles.
	 */
	if (memberships)
	{
		rel = table_open(AuthMemRelationId, AccessShareLock);
		ScanKeyInit(&scankey,
					Anum_pg_auth_members_member,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));
		scan = systable_beginscan(rel, AuthMemMemRoleIndexId, true,
								  NULL, 1, &scankey);

		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Form_pg_auth_members memform = (Form_pg_auth_members) GETSTRUCT(tuple);
			char	   *granted_role;
			char	   *grantor;

			granted_role = GetUserNameFromId(memform->roleid, false);
			grantor = GetUserNameFromId(memform->grantor, false);

			resetStringInfo(&buf);
			appendStringInfo(&buf, "GRANT %s TO %s",
							 quote_identifier(granted_role),
							 quote_identifier(rolname));
			appendStringInfo(&buf, " WITH ADMIN %s, INHERIT %s, SET %s",
							 memform->admin_option ? "TRUE" : "FALSE",
							 memform->inherit_option ? "TRUE" : "FALSE",
							 memform->set_option ? "TRUE" : "FALSE");
			appendStringInfo(&buf, " GRANTED BY %s;",
							 quote_identifier(grantor));

			statements = lappend(statements, pstrdup(buf.data));

			pfree(granted_role);
			pfree(grantor);
		}

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}

	pfree(buf.data);
	pfree(rolname);

	return statements;
}

/*
 * pg_get_role_ddl
 *		Return DDL to recreate a role as a set of text rows.
 *
 * Each row is a complete SQL statement.  The first row is always the
 * CREATE ROLE statement; subsequent rows are ALTER ROLE SET statements
 * and optionally GRANT statements for role memberships.
 * Returns no rows if the role argument is NULL.
 */
Datum
pg_get_role_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			roleid;
		DdlOption	opts[] = {
			{"pretty", DDL_OPT_BOOL},
			{"memberships", DDL_OPT_BOOL},
		};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		roleid = PG_GETARG_OID(0);
		parse_ddl_options(fcinfo, 1, opts, lengthof(opts));

		statements = pg_get_role_ddl_internal(roleid,
											  opts[0].isset && opts[0].boolval,
											  !opts[1].isset || opts[1].boolval);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_get_tablespace_ddl_internal
 *		Generate DDL statements to recreate a tablespace.
 *
 * Returns a List of palloc'd strings.  The first element is the
 * CREATE TABLESPACE statement; if the tablespace has reloptions,
 * a second element with ALTER TABLESPACE SET (...) is appended.
 */
static List *
pg_get_tablespace_ddl_internal(Oid tsid, bool pretty, bool no_owner)
{
	HeapTuple	tuple;
	Form_pg_tablespace tspForm;
	StringInfoData buf;
	char	   *spcname;
	char	   *spcowner;
	char	   *path;
	bool		isNull;
	Datum		datum;
	List	   *statements = NIL;

	tuple = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(tsid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist",
						tsid)));

	tspForm = (Form_pg_tablespace) GETSTRUCT(tuple);
	spcname = pstrdup(NameStr(tspForm->spcname));

	/* User must have SELECT privilege on pg_tablespace. */
	if (pg_class_aclcheck(TableSpaceRelationId, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
	{
		ReleaseSysCache(tuple);
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE, spcname);
	}

	/*
	 * We don't support generating DDL for system tablespaces.  The primary
	 * reason for this is that users shouldn't be recreating them.
	 */
	if (IsReservedName(spcname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("tablespace name \"%s\" is reserved", spcname),
				 errdetail("Tablespace names starting with \"pg_\" are reserved for system tablespaces.")));

	initStringInfo(&buf);

	/* Start building the CREATE TABLESPACE statement */
	appendStringInfo(&buf, "CREATE TABLESPACE %s", quote_identifier(spcname));

	/* Add OWNER clause */
	if (!no_owner)
	{
		spcowner = GetUserNameFromId(tspForm->spcowner, false);
		append_ddl_option(&buf, pretty, 4, "OWNER %s",
						  quote_identifier(spcowner));
		pfree(spcowner);
	}

	/* Find tablespace directory path */
	path = get_tablespace_location(tsid);

	/* Add directory LOCATION (path), if it exists */
	if (path[0] != '\0')
	{
		/*
		 * Special case: if the tablespace was created with GUC
		 * "allow_in_place_tablespaces = true" and "LOCATION ''", path will
		 * begin with "pg_tblspc/". In that case, show "LOCATION ''" as the
		 * user originally specified.
		 */
		if (strncmp(PG_TBLSPC_DIR_SLASH, path, strlen(PG_TBLSPC_DIR_SLASH)) == 0)
			append_ddl_option(&buf, pretty, 4, "LOCATION ''");
		else
			append_ddl_option(&buf, pretty, 4, "LOCATION %s",
							  quote_literal_cstr(path));
	}
	pfree(path);

	appendStringInfoChar(&buf, ';');
	statements = lappend(statements, pstrdup(buf.data));

	/* Check for tablespace options */
	datum = SysCacheGetAttr(TABLESPACEOID, tuple,
							Anum_pg_tablespace_spcoptions, &isNull);
	if (!isNull)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER TABLESPACE %s SET (",
						 quote_identifier(spcname));
		get_reloptions(&buf, datum);
		appendStringInfoString(&buf, ");");
		statements = lappend(statements, pstrdup(buf.data));
	}

	ReleaseSysCache(tuple);
	pfree(spcname);
	pfree(buf.data);

	return statements;
}

/*
 * pg_get_tablespace_ddl_srf - common SRF logic for tablespace DDL
 */
static Datum
pg_get_tablespace_ddl_srf(FunctionCallInfo fcinfo, Oid tsid, bool isnull)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		DdlOption	opts[] = {
			{"pretty", DDL_OPT_BOOL},
			{"owner", DDL_OPT_BOOL},
		};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (isnull)
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		parse_ddl_options(fcinfo, 1, opts, lengthof(opts));

		statements = pg_get_tablespace_ddl_internal(tsid,
													opts[0].isset && opts[0].boolval,
													opts[1].isset && !opts[1].boolval);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = (char *) list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_get_tablespace_ddl_oid
 *		Return DDL to recreate a tablespace, taking OID.
 */
Datum
pg_get_tablespace_ddl_oid(PG_FUNCTION_ARGS)
{
	Oid			tsid = InvalidOid;
	bool		isnull;

	isnull = PG_ARGISNULL(0);
	if (!isnull)
		tsid = PG_GETARG_OID(0);

	return pg_get_tablespace_ddl_srf(fcinfo, tsid, isnull);
}

/*
 * pg_get_tablespace_ddl_name
 *		Return DDL to recreate a tablespace, taking name.
 */
Datum
pg_get_tablespace_ddl_name(PG_FUNCTION_ARGS)
{
	Oid			tsid = InvalidOid;
	Name		tspname;
	bool		isnull;

	isnull = PG_ARGISNULL(0);

	if (!isnull)
	{
		tspname = PG_GETARG_NAME(0);
		tsid = get_tablespace_oid(NameStr(*tspname), false);
	}

	return pg_get_tablespace_ddl_srf(fcinfo, tsid, isnull);
}

/*
 * pg_get_database_ddl_internal
 *		Generate DDL statements to recreate a database.
 *
 * Returns a List of palloc'd strings.  The first element is the
 * CREATE DATABASE statement; subsequent elements are ALTER DATABASE
 * statements for properties and configuration settings.
 */
static List *
pg_get_database_ddl_internal(Oid dbid, bool pretty,
							 bool no_owner, bool no_tablespace)
{
	HeapTuple	tuple;
	Form_pg_database dbform;
	StringInfoData buf;
	bool		isnull;
	Datum		datum;
	const char *encoding;
	char	   *dbname;
	char	   *collate;
	char	   *ctype;
	Relation	rel;
	ScanKeyData scankey[2];
	SysScanDesc scan;
	List	   *statements = NIL;
	AclResult	aclresult;

	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("database with OID %u does not exist", dbid)));

	/* User must have connect privilege for target database. */
	aclresult = object_aclcheck(DatabaseRelationId, dbid, GetUserId(), ACL_CONNECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(dbid));

	dbform = (Form_pg_database) GETSTRUCT(tuple);
	dbname = pstrdup(NameStr(dbform->datname));

	/*
	 * Reject invalid databases. Deparsing a pg_database row in invalid state
	 * can produce SQL that is not executable, such as CONNECTION LIMIT = -2.
	 */
	if (database_is_invalid_form(dbform))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot generate DDL for invalid database \"%s\"",
						dbname)));

	/*
	 * We don't support generating DDL for system databases.  The primary
	 * reason for this is that users shouldn't be recreating them.
	 */
	if (strcmp(dbname, "template0") == 0 || strcmp(dbname, "template1") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("database \"%s\" is a system database", dbname),
				 errdetail("DDL generation is not supported for template0 and template1.")));

	initStringInfo(&buf);

	/* --- Build CREATE DATABASE statement --- */
	appendStringInfo(&buf, "CREATE DATABASE %s", quote_identifier(dbname));

	/*
	 * Always use template0: the target database already contains the catalog
	 * data from whatever template was used originally, so we must start from
	 * the pristine template to avoid duplication.
	 */
	append_ddl_option(&buf, pretty, 4, "WITH TEMPLATE = template0");

	/* ENCODING */
	encoding = pg_encoding_to_char(dbform->encoding);
	if (strlen(encoding) > 0)
		append_ddl_option(&buf, pretty, 4, "ENCODING = %s",
						  quote_literal_cstr(encoding));

	/* LOCALE_PROVIDER */
	if (dbform->datlocprovider == COLLPROVIDER_BUILTIN ||
		dbform->datlocprovider == COLLPROVIDER_ICU ||
		dbform->datlocprovider == COLLPROVIDER_LIBC)
		append_ddl_option(&buf, pretty, 4, "LOCALE_PROVIDER = %s",
						  collprovider_name(dbform->datlocprovider));
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("unrecognized locale provider: %c",
						dbform->datlocprovider)));

	/* LOCALE, LC_COLLATE, LC_CTYPE */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datcollate, &isnull);
	collate = isnull ? NULL : TextDatumGetCString(datum);
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datctype, &isnull);
	ctype = isnull ? NULL : TextDatumGetCString(datum);
	if (collate != NULL && ctype != NULL && strcmp(collate, ctype) == 0)
	{
		append_ddl_option(&buf, pretty, 4, "LOCALE = %s",
						  quote_literal_cstr(collate));
	}
	else
	{
		if (collate != NULL)
			append_ddl_option(&buf, pretty, 4, "LC_COLLATE = %s",
							  quote_literal_cstr(collate));
		if (ctype != NULL)
			append_ddl_option(&buf, pretty, 4, "LC_CTYPE = %s",
							  quote_literal_cstr(ctype));
	}

	/* LOCALE (provider-specific) */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datlocale, &isnull);
	if (!isnull)
	{
		const char *locale = TextDatumGetCString(datum);

		if (dbform->datlocprovider == COLLPROVIDER_BUILTIN)
			append_ddl_option(&buf, pretty, 4, "BUILTIN_LOCALE = %s",
							  quote_literal_cstr(locale));
		else if (dbform->datlocprovider == COLLPROVIDER_ICU)
			append_ddl_option(&buf, pretty, 4, "ICU_LOCALE = %s",
							  quote_literal_cstr(locale));
	}

	/* ICU_RULES */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_daticurules, &isnull);
	if (!isnull && dbform->datlocprovider == COLLPROVIDER_ICU)
		append_ddl_option(&buf, pretty, 4, "ICU_RULES = %s",
						  quote_literal_cstr(TextDatumGetCString(datum)));

	/* TABLESPACE */
	if (!no_tablespace && OidIsValid(dbform->dattablespace))
	{
		char	   *spcname = get_tablespace_name(dbform->dattablespace);

		if (spcname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace with OID %u does not exist",
							dbform->dattablespace),
					 errdetail("It may have been concurrently dropped.")));

		if (pg_strcasecmp(spcname, "pg_default") != 0)
			append_ddl_option(&buf, pretty, 4, "TABLESPACE = %s",
							  quote_identifier(spcname));
	}

	appendStringInfoChar(&buf, ';');
	statements = lappend(statements, pstrdup(buf.data));

	/* OWNER */
	if (!no_owner && OidIsValid(dbform->datdba))
	{
		char	   *owner = GetUserNameFromId(dbform->datdba, false);

		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s OWNER TO %s;",
						 quote_identifier(dbname), quote_identifier(owner));
		pfree(owner);
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* CONNECTION LIMIT */
	if (dbform->datconnlimit != -1)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s CONNECTION LIMIT = %d;",
						 quote_identifier(dbname), dbform->datconnlimit);
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* IS_TEMPLATE */
	if (dbform->datistemplate)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s IS_TEMPLATE = true;",
						 quote_identifier(dbname));
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* ALLOW_CONNECTIONS */
	if (!dbform->datallowconn)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s ALLOW_CONNECTIONS = false;",
						 quote_identifier(dbname));
		statements = lappend(statements, pstrdup(buf.data));
	}

	ReleaseSysCache(tuple);

	/*
	 * Now scan pg_db_role_setting for ALTER DATABASE SET configurations.
	 *
	 * It is only database-wide (setrole = 0). It generates one ALTER
	 * statement per setting.
	 */
	rel = table_open(DbRoleSettingRelationId, AccessShareLock);
	ScanKeyInit(&scankey[0],
				Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));
	ScanKeyInit(&scankey[1],
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));

	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 2, scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ArrayType  *dbconfig;
		Datum	   *settings;
		bool	   *nulls;
		int			nsettings;

		/*
		 * The setconfig column is a text array in "name=value" format. It
		 * should never be null for a valid row, but be defensive.
		 */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		if (isnull)
			continue;

		dbconfig = DatumGetArrayTypePCopy(datum);

		deconstruct_array_builtin(dbconfig, TEXTOID, &settings, &nulls, &nsettings);

		for (int i = 0; i < nsettings; i++)
		{
			char	   *s,
					   *p;

			if (nulls[i])
				continue;

			s = TextDatumGetCString(settings[i]);
			p = strchr(s, '=');
			if (p == NULL)
			{
				pfree(s);
				continue;
			}
			*p++ = '\0';

			resetStringInfo(&buf);
			appendStringInfo(&buf, "ALTER DATABASE %s SET %s TO ",
							 quote_identifier(dbname),
							 quote_identifier(s));

			append_guc_value(&buf, s, p);

			appendStringInfoChar(&buf, ';');

			statements = lappend(statements, pstrdup(buf.data));

			pfree(s);
		}

		pfree(settings);
		pfree(nulls);
		pfree(dbconfig);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	pfree(buf.data);
	pfree(dbname);

	return statements;
}

/*
 * pg_get_database_ddl
 *		Return DDL to recreate a database as a set of text rows.
 */
Datum
pg_get_database_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			dbid;
		DdlOption	opts[] = {
			{"pretty", DDL_OPT_BOOL},
			{"owner", DDL_OPT_BOOL},
			{"tablespace", DDL_OPT_BOOL},
		};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		dbid = PG_GETARG_OID(0);
		parse_ddl_options(fcinfo, 1, opts, lengthof(opts));

		statements = pg_get_database_ddl_internal(dbid,
												  opts[0].isset && opts[0].boolval,
												  opts[1].isset && !opts[1].boolval,
												  opts[2].isset && !opts[2].boolval);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * get_inheritance_parents
 *		Return a List of parent OIDs for relid, ordered by inhseqno.
 *
 * find_inheritance_children() walks the opposite direction (parent->children),
 * so we scan pg_inherits directly here using the (inhrelid, inhseqno) index,
 * which yields rows in the order they need to appear in the INHERITS clause.
 * Partition children also have a pg_inherits entry, so callers must skip the
 * INHERITS clause when relispartition is true.
 */
static List *
get_inheritance_parents(Oid relid)
{
	Relation	inheritsRel;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;
	List	   *parents = NIL;

	inheritsRel = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(inheritsRel, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(tup);

		parents = lappend_oid(parents, inh->inhparent);
	}
	systable_endscan(scan);
	table_close(inheritsRel, AccessShareLock);

	return parents;
}

/*
 * lookup_qualified_relname
 *		Return the schema-qualified, identifier-quoted name of a relation,
 *		or raise ERRCODE_UNDEFINED_OBJECT if the relation has disappeared.
 *
 * Replaces the unsafe pattern
 *	  quote_qualified_identifier(get_namespace_name(get_rel_namespace(oid)),
 *	                             get_rel_name(oid))
 * which dereferences NULL when a concurrent transaction has dropped the
 * referenced relation (or its schema) between when we cached its OID and
 * when we ask the syscache for its name.  Holding AccessShareLock on a
 * dependent relation makes this race vanishingly unlikely in practice,
 * but we still defend against it because the alternative is a SIGSEGV.
 *
 * Caller is responsible for pfree()ing the result.
 */
static char *
lookup_qualified_relname(Oid relid)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("relation with OID %u does not exist", relid),
				 errdetail("It may have been concurrently dropped.")));

	reltup = (Form_pg_class) GETSTRUCT(tp);
	nspname = get_namespace_name(reltup->relnamespace);
	if (nspname == NULL)
	{
		Oid			nspoid = reltup->relnamespace;

		ReleaseSysCache(tp);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("schema with OID %u does not exist", nspoid),
				 errdetail("It may have been concurrently dropped.")));
	}

	result = quote_qualified_identifier(nspname, NameStr(reltup->relname));

	pfree(nspname);
	ReleaseSysCache(tp);

	return result;
}

/*
 * find_attrdef_text
 *		Return the deparsed DEFAULT/GENERATED expression for attnum on rel,
 *		or NULL if no entry exists in TupleConstr->defval.
 *
 * The caller passes a List ** so that the deparse context is built lazily
 * and reused across calls (deparse_context_for is not cheap).  Returned
 * string is palloc'd in the current memory context; caller pfree's it.
 */
static char *
find_attrdef_text(Relation rel, AttrNumber attnum, List **dpcontext)
{
	TupleConstr *constr = RelationGetDescr(rel)->constr;

	if (constr == NULL)
		return NULL;

	for (int j = 0; j < constr->num_defval; j++)
	{
		if (constr->defval[j].adnum != attnum)
			continue;

		if (*dpcontext == NIL)
			*dpcontext = deparse_context_for(RelationGetRelationName(rel),
											 RelationGetRelid(rel));

		return deparse_expression(stringToNode(constr->defval[j].adbin),
								  *dpcontext, false, false);
	}
	return NULL;
}

/*
 * append_inline_check_constraints
 *		Emit each locally-declared CHECK constraint on rel as
 *		"CONSTRAINT name <pg_get_constraintdef>", separated by ',' from any
 *		previously-emitted column or constraint.
 *
 * *first tracks whether anything has been emitted on this list yet, so the
 * caller can chain column emission and constraint emission through the same
 * buffer.  Inherited CHECK constraints (!conislocal) come from the parent's
 * DDL and aren't repeated here.
 */
static void
append_inline_check_constraints(StringInfo buf, Relation rel, bool pretty,
								bool *first)
{
	Relation	conRel;
	SysScanDesc conScan;
	ScanKeyData conKey;
	HeapTuple	conTup;

	conRel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&conKey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	conScan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId,
								 true, NULL, 1, &conKey);

	while (HeapTupleIsValid(conTup = systable_getnext(conScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);
		Datum		defDatum;
		char	   *defbody;

		if (con->contype != CONSTRAINT_CHECK)
			continue;
		if (!con->conislocal)
			continue;

		if (!*first)
			appendStringInfoChar(buf, ',');
		*first = false;
		if (pretty)
			appendStringInfoString(buf, "\n    ");
		else
			appendStringInfoChar(buf, ' ');

		defDatum = OidFunctionCall1(F_PG_GET_CONSTRAINTDEF_OID,
									ObjectIdGetDatum(con->oid));
		defbody = TextDatumGetCString(defDatum);
		appendStringInfo(buf, "CONSTRAINT %s %s",
						 quote_identifier(NameStr(con->conname)),
						 defbody);
		pfree(defbody);
	}
	systable_endscan(conScan);
	table_close(conRel, AccessShareLock);
}

/*
 * append_column_defs
 *		Append the comma-separated column definition list for a table.
 *
 * Emits each non-dropped, locally-declared column as
 *		name type [COLLATE x] [STORAGE s] [COMPRESSION c]
 *		[GENERATED ... | DEFAULT e] [NOT NULL]
 * followed by any locally-declared inline CHECK constraints.  Optional
 * clauses are omitted when their value matches what the system would
 * reapply on round-trip (e.g. type-default COLLATE, type-default STORAGE).
 */
static void
append_column_defs(StringInfo buf, Relation rel, bool pretty,
				   bool include_constraints)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	List	   *dpcontext = NIL;
	bool		first = true;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *typstr;

		if (att->attisdropped)
			continue;

		/*
		 * Columns inherited from a parent are emitted by the INHERITS clause
		 * (once implemented), not the column list, unless the child
		 * redeclared them locally (attislocal=true).
		 */
		if (!att->attislocal)
			continue;

		if (!first)
			appendStringInfoChar(buf, ',');
		first = false;

		if (pretty)
			appendStringInfoString(buf, "\n    ");
		else
			appendStringInfoChar(buf, ' ');

		appendStringInfoString(buf, quote_identifier(NameStr(att->attname)));
		appendStringInfoChar(buf, ' ');

		typstr = format_type_with_typemod(att->atttypid, att->atttypmod);
		appendStringInfoString(buf, typstr);
		pfree(typstr);

		/* COLLATE clause, only if it differs from the type's default. */
		if (OidIsValid(att->attcollation) &&
			att->attcollation != get_typcollation(att->atttypid))
			appendStringInfo(buf, " COLLATE %s",
							 generate_collation_name(att->attcollation));

		/* STORAGE clause, only if it differs from the type's default. */
		if (att->attstorage != get_typstorage(att->atttypid))
		{
			const char *storage = NULL;

			switch (att->attstorage)
			{
				case TYPSTORAGE_PLAIN:
					storage = "PLAIN";
					break;
				case TYPSTORAGE_EXTERNAL:
					storage = "EXTERNAL";
					break;
				case TYPSTORAGE_MAIN:
					storage = "MAIN";
					break;
				case TYPSTORAGE_EXTENDED:
					storage = "EXTENDED";
					break;
			}
			if (storage)
				appendStringInfo(buf, " STORAGE %s", storage);
		}

		/* COMPRESSION clause, only if explicitly set on the column. */
		if (CompressionMethodIsValid(att->attcompression))
		{
			const char *cm = NULL;

			switch (att->attcompression)
			{
				case TOAST_PGLZ_COMPRESSION:
					cm = "pglz";
					break;
				case TOAST_LZ4_COMPRESSION:
					cm = "lz4";
					break;
			}
			if (cm)
				appendStringInfo(buf, " COMPRESSION %s", cm);
		}

		/*
		 * Look up the default/generated expression text up front; generated
		 * columns have atthasdef=true with an entry in pg_attrdef just like
		 * regular defaults.
		 */
		{
			char	   *defexpr = NULL;

			if (att->atthasdef)
				defexpr = find_attrdef_text(rel, att->attnum, &dpcontext);

			/* GENERATED / IDENTITY / DEFAULT are mutually exclusive. */
			if (att->attgenerated == ATTRIBUTE_GENERATED_STORED && defexpr)
				appendStringInfo(buf, " GENERATED ALWAYS AS (%s) STORED", defexpr);
			else if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL && defexpr)
				appendStringInfo(buf, " GENERATED ALWAYS AS (%s) VIRTUAL", defexpr);
			else if (att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS ||
					 att->attidentity == ATTRIBUTE_IDENTITY_BY_DEFAULT)
			{
				const char *idkind =
					(att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS)
					? "ALWAYS" : "BY DEFAULT";
				Oid			seqid = getIdentitySequence(rel, att->attnum, true);

				appendStringInfo(buf, " GENERATED %s AS IDENTITY", idkind);

				/*
				 * Emit only the sequence options that differ from their
				 * defaults — mirroring pg_get_database_ddl's pattern of
				 * omitting values that the system would reapply on its own.
				 */
				if (OidIsValid(seqid))
				{
					HeapTuple	seqTup = SearchSysCache1(SEQRELID,
														 ObjectIdGetDatum(seqid));

					if (HeapTupleIsValid(seqTup))
					{
						Form_pg_sequence seq = (Form_pg_sequence) GETSTRUCT(seqTup);
						StringInfoData opts;
						bool		first_opt = true;
						int64		def_min,
									def_max,
									def_start;
						int64		typ_min,
									typ_max;

						/*
						 * Per-type bounds for the sequence's underlying
						 * integer type.  Defaults to int8 if the column type
						 * is something else (shouldn't happen for IDENTITY,
						 * but be defensive).
						 */
						switch (att->atttypid)
						{
							case INT2OID:
								typ_min = PG_INT16_MIN;
								typ_max = PG_INT16_MAX;
								break;
							case INT4OID:
								typ_min = PG_INT32_MIN;
								typ_max = PG_INT32_MAX;
								break;
							default:
								typ_min = PG_INT64_MIN;
								typ_max = PG_INT64_MAX;
								break;
						}

						if (seq->seqincrement > 0)
						{
							def_min = 1;
							def_max = typ_max;
							def_start = def_min;
						}
						else
						{
							def_min = typ_min;
							def_max = -1;
							def_start = def_max;
						}

						initStringInfo(&opts);

						/*
						 * SEQUENCE NAME — omit when it matches the
						 * implicit "<tablename>_<columnname>_seq" pattern
						 * in the same schema, since CREATE TABLE will
						 * regenerate that exact name.  The sequence is an
						 * INTERNAL dependency of the column, so the lock
						 * we hold on the table also pins it, but the
						 * lookup helper still defends against a missing
						 * pg_class row.
						 */
						{
							HeapTuple	seqClassTup;
							Form_pg_class seqClass;
							char		autoname[NAMEDATALEN];

							seqClassTup = SearchSysCache1(RELOID,
														  ObjectIdGetDatum(seqid));
							if (!HeapTupleIsValid(seqClassTup))
								ereport(ERROR,
										(errcode(ERRCODE_UNDEFINED_OBJECT),
										 errmsg("identity sequence with OID %u does not exist",
												seqid),
										 errdetail("It may have been concurrently dropped.")));
							seqClass = (Form_pg_class) GETSTRUCT(seqClassTup);

							snprintf(autoname, sizeof(autoname), "%s_%s_seq",
									 RelationGetRelationName(rel),
									 NameStr(att->attname));
							if (seqClass->relnamespace != RelationGetNamespace(rel) ||
								strcmp(NameStr(seqClass->relname), autoname) != 0)
							{
								char	   *seqQual =
									lookup_qualified_relname(seqid);

								appendStringInfo(&opts, "%sSEQUENCE NAME %s",
												 first_opt ? "" : " ", seqQual);
								first_opt = false;
								pfree(seqQual);
							}
							ReleaseSysCache(seqClassTup);
						}

						if (seq->seqstart != def_start)
						{
							appendStringInfo(&opts, "%sSTART WITH " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqstart);
							first_opt = false;
						}
						if (seq->seqincrement != 1)
						{
							appendStringInfo(&opts, "%sINCREMENT BY " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqincrement);
							first_opt = false;
						}
						if (seq->seqmin != def_min)
						{
							appendStringInfo(&opts, "%sMINVALUE " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqmin);
							first_opt = false;
						}
						if (seq->seqmax != def_max)
						{
							appendStringInfo(&opts, "%sMAXVALUE " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqmax);
							first_opt = false;
						}
						if (seq->seqcache != 1)
						{
							appendStringInfo(&opts, "%sCACHE " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqcache);
							first_opt = false;
						}
						if (seq->seqcycle)
						{
							appendStringInfo(&opts, "%sCYCLE", first_opt ? "" : " ");
							first_opt = false;
						}

						if (!first_opt)
							appendStringInfo(buf, " (%s)", opts.data);

						pfree(opts.data);
						ReleaseSysCache(seqTup);
					}
				}
			}
			else if (defexpr)
				appendStringInfo(buf, " DEFAULT %s", defexpr);

			if (defexpr)
				pfree(defexpr);
		}

		if (att->attnotnull)
			appendStringInfoString(buf, " NOT NULL");
	}

	/*
	 * Table-level CHECK constraints — emitted inline in the CREATE TABLE
	 * body so they appear alongside the columns (the pg_dump shape).  The
	 * constraint loop later in pg_get_table_ddl_internal skips CHECK
	 * constraints to avoid double-emission.
	 */
	if (include_constraints)
		append_inline_check_constraints(buf, rel, pretty, &first);
}

/*
 * append_typed_column_overrides
 *		For a typed table (CREATE TABLE ... OF type_name), append the
 *		optional "(col WITH OPTIONS ..., ...)" list carrying locally
 *		applied per-column overrides — DEFAULT, NOT NULL, and any locally
 *		declared CHECK constraints.
 *
 * Columns whose type is fully dictated by reloftype emit nothing.  The
 * parenthesised list is suppressed entirely when no column needs an
 * override and there are no locally-declared CHECK constraints, matching
 * the canonical "CREATE TABLE x OF t;" shape.
 */
static void
append_typed_column_overrides(StringInfo buf, Relation rel, bool pretty,
							  bool include_constraints)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	List	   *dpcontext = NIL;
	StringInfoData inner;
	bool		first = true;

	initStringInfo(&inner);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *defexpr = NULL;
		bool		has_default;
		bool		has_notnull;

		if (att->attisdropped)
			continue;

		if (att->atthasdef)
			defexpr = find_attrdef_text(rel, att->attnum, &dpcontext);

		has_default = (defexpr != NULL);
		has_notnull = att->attnotnull;

		if (!has_default && !has_notnull)
		{
			if (defexpr)
				pfree(defexpr);
			continue;
		}

		if (!first)
			appendStringInfoChar(&inner, ',');
		first = false;
		if (pretty)
			appendStringInfoString(&inner, "\n    ");
		else
			appendStringInfoChar(&inner, ' ');

		appendStringInfo(&inner, "%s WITH OPTIONS",
						 quote_identifier(NameStr(att->attname)));
		if (has_default)
			appendStringInfo(&inner, " DEFAULT %s", defexpr);
		if (has_notnull)
			appendStringInfoString(&inner, " NOT NULL");

		if (defexpr)
			pfree(defexpr);
	}

	/*
	 * Locally-declared CHECK constraints on a typed table belong in the
	 * column-list parentheses, same as for an untyped table.  The
	 * out-of-line constraint loop later still skips CHECKs.
	 */
	if (include_constraints)
		append_inline_check_constraints(&inner, rel, pretty, &first);

	if (!first)
	{
		appendStringInfoString(buf, " (");
		appendStringInfoString(buf, inner.data);
		if (pretty)
			appendStringInfoString(buf, "\n)");
		else
			appendStringInfoChar(buf, ')');
	}
	pfree(inner.data);
}

/*
 * pg_get_table_ddl_internal
 *		Generate DDL statements to recreate a regular or partitioned table.
 *
 * The first list element is the CREATE TABLE statement.  Subsequent
 * elements are the ALTER TABLE / CREATE INDEX / CREATE RULE /
 * CREATE STATISTICS statements needed to restore the table's full
 * definition.
 *
 * Trigger and policy emission are scaffolded but not yet wired up — they
 * are gated on standalone pg_get_trigger_ddl / pg_get_policy_ddl helpers
 * landing.
 */
static List *
pg_get_table_ddl_internal(Oid relid, bool pretty,
						  bool no_owner, bool no_tablespace,
						  bool include_indexes,
						  bool include_constraints,
						  bool include_rules,
						  bool include_statistics,
						  bool include_triggers,
						  bool include_policies,
						  bool include_rls,
						  bool include_replica_identity,
						  bool include_partition)
{
	Relation	rel;
	StringInfoData buf;
	List	   *statements = NIL;
	char	   *qualname;
	char		relkind;
	char		relpersistence;
	bool		is_typed;
	AclResult	aclresult;

	rel = table_open(relid, AccessShareLock);

	relkind = rel->rd_rel->relkind;
	relpersistence = rel->rd_rel->relpersistence;
	is_typed = OidIsValid(rel->rd_rel->reloftype);

	/*
	 * The initial cut only supports ordinary and partitioned tables.  Views,
	 * matviews, foreign tables, sequences, indexes, composite types, and
	 * TOAST tables are out of scope for now.
	 */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
	{
		char	   *relname = pstrdup(RelationGetRelationName(rel));

		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an ordinary or partitioned table",
						relname)));
	}

	/* Caller needs SELECT on the table to read its definition. */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_TABLE,
					   RelationGetRelationName(rel));

	qualname = lookup_qualified_relname(relid);

	initStringInfo(&buf);

	/* pg_class tuple — for relpartbound and reloptions */
	{
		HeapTuple	classtup;
		Datum		reloptDatum;
		bool		reloptIsnull;

		classtup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(classtup))
			elog(ERROR, "cache lookup failed for relation %u", relid);

		reloptDatum = SysCacheGetAttr(RELOID, classtup,
									  Anum_pg_class_reloptions, &reloptIsnull);

		/*
		 * CREATE [TEMPORARY | UNLOGGED] TABLE qualname ...
		 *
		 * Persistence applies uniformly to all three CREATE TABLE forms
		 * (regular column list, OF type_name, and PARTITION OF).  Only one
		 * of TEMPORARY / UNLOGGED can be set; the relpersistence catalog
		 * field is the single source of truth.
		 */
		appendStringInfoString(&buf, "CREATE ");
		if (relpersistence == RELPERSISTENCE_TEMP)
			appendStringInfoString(&buf, "TEMPORARY ");
		else if (relpersistence == RELPERSISTENCE_UNLOGGED)
			appendStringInfoString(&buf, "UNLOGGED ");
		appendStringInfo(&buf, "TABLE %s", qualname);

		if (rel->rd_rel->relispartition)
		{
			/* PARTITION OF parent FOR VALUES ... */
			Oid			parentOid = get_partition_parent(relid, true);
			char	   *parentQual = lookup_qualified_relname(parentOid);
			char	   *parentRelname = get_rel_name(parentOid);
			Datum		boundDatum;
			bool		boundIsnull;
			char	   *forValues = NULL;
			char	   *boundStr = NULL;

			if (parentRelname == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("partition parent with OID %u does not exist",
								parentOid),
						 errdetail("It may have been concurrently dropped.")));

			boundDatum = SysCacheGetAttr(RELOID, classtup,
										 Anum_pg_class_relpartbound, &boundIsnull);
			if (!boundIsnull)
			{
				Node	   *boundNode;
				List	   *dpcontext;

				boundStr = TextDatumGetCString(boundDatum);
				boundNode = stringToNode(boundStr);
				dpcontext = deparse_context_for(parentRelname, parentOid);
				forValues = deparse_expression(boundNode, dpcontext, false, false);
			}

			appendStringInfo(&buf, " PARTITION OF %s %s",
							 parentQual, forValues ? forValues : "DEFAULT");
			if (forValues)
				pfree(forValues);
			if (boundStr)
				pfree(boundStr);
			pfree(parentQual);
			pfree(parentRelname);

			/*
			 * Column-level overrides redeclared on the child are emitted
			 * out-of-line: NOT NULL/CHECK come through the constraint loop
			 * (conislocal=true), and DEFAULT comes through the dedicated
			 * ALTER COLUMN SET DEFAULT pass below.
			 */
		}
		else if (is_typed)
		{
			/*
			 * Typed table: CREATE TABLE name OF type_name [(col WITH
			 * OPTIONS ...)].  The column list (when present) carries only
			 * locally-applied overrides — defaults, NOT NULL toggles, and
			 * locally-declared CHECK constraints — for columns whose type
			 * is otherwise dictated by reloftype.
			 */
			char	   *typname = format_type_be_qualified(rel->rd_rel->reloftype);

			appendStringInfo(&buf, " OF %s", typname);
			pfree(typname);

			append_typed_column_overrides(&buf, rel, pretty, include_constraints);
		}
		else
		{
			List	   *parents;
			ListCell   *lc;
			bool		first;

			appendStringInfoString(&buf, " (");

			append_column_defs(&buf, rel, pretty, include_constraints);

			if (pretty)
				appendStringInfoString(&buf, "\n)");
			else
				appendStringInfoChar(&buf, ')');

			/* INHERITS (parent1, parent2, ...) — non-partition inheritance only */
			parents = get_inheritance_parents(relid);
			if (parents != NIL)
			{
				appendStringInfoString(&buf, " INHERITS (");
				first = true;
				foreach(lc, parents)
				{
					Oid			poid = lfirst_oid(lc);
					char	   *pname = lookup_qualified_relname(poid);

					if (!first)
						appendStringInfoString(&buf, ", ");
					first = false;
					appendStringInfoString(&buf, pname);
					pfree(pname);
				}
				appendStringInfoChar(&buf, ')');
				list_free(parents);
			}
		}

		/* PARTITION BY — applies whenever this relation is a partitioned table */
		if (relkind == RELKIND_PARTITIONED_TABLE)
		{
			Datum		partkeyDatum;
			char	   *partkey;

			partkeyDatum = OidFunctionCall1(F_PG_GET_PARTKEYDEF,
											ObjectIdGetDatum(relid));
			partkey = TextDatumGetCString(partkeyDatum);
			appendStringInfo(&buf, " PARTITION BY %s", partkey);
			pfree(partkey);
		}

		/*
		 * USING method — emit only when the table access method differs
		 * from heap (the cluster default).  Pluggable table AMs have been
		 * supported since PostgreSQL 12.
		 */
		if (OidIsValid(rel->rd_rel->relam) &&
			rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		{
			char	   *amname = get_am_name(rel->rd_rel->relam);

			if (amname != NULL)
			{
				appendStringInfo(&buf, " USING %s", quote_identifier(amname));
				pfree(amname);
			}
		}

		/* WITH (reloptions) */
		if (!reloptIsnull)
		{
			appendStringInfoString(&buf, " WITH (");
			get_reloptions(&buf, reloptDatum);
			appendStringInfoChar(&buf, ')');
		}

		ReleaseSysCache(classtup);
	}

	/* TABLESPACE */
	if (!no_tablespace && OidIsValid(rel->rd_rel->reltablespace))
	{
		char	   *tsname = get_tablespace_name(rel->rd_rel->reltablespace);

		if (tsname != NULL)
		{
			appendStringInfo(&buf, " TABLESPACE %s", quote_identifier(tsname));
			pfree(tsname);
		}
	}

	/*
	 * ON COMMIT applies only to temporary tables.  The action lives in a
	 * backend-local list (not the catalog) since it's a session-scoped
	 * property, so this is best-effort: we can only see entries registered
	 * in the current backend.  PRESERVE ROWS is the default and is not
	 * emitted; NOOP indicates no entry was found.
	 */
	if (relpersistence == RELPERSISTENCE_TEMP)
	{
		OnCommitAction oc = get_on_commit_action(relid);

		if (oc == ONCOMMIT_DELETE_ROWS)
			appendStringInfoString(&buf, " ON COMMIT DELETE ROWS");
		else if (oc == ONCOMMIT_DROP)
			appendStringInfoString(&buf, " ON COMMIT DROP");
	}

	appendStringInfoChar(&buf, ';');
	statements = lappend(statements, pstrdup(buf.data));

	/* OWNER */
	if (!no_owner)
	{
		char	   *owner = GetUserNameFromId(rel->rd_rel->relowner, false);

		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER TABLE %s OWNER TO %s;",
						 qualname, quote_identifier(owner));
		statements = lappend(statements, pstrdup(buf.data));
		pfree(owner);
	}

	/*
	 * Per-column DEFAULT overrides on inherited/partition children.  The
	 * column list inside CREATE TABLE only emits locally-declared columns
	 * (attislocal=true), so any default set locally on a column that came
	 * from a parent table needs to be re-applied with ALTER COLUMN SET
	 * DEFAULT.  Note: NOT NULL overrides come out through the constraint
	 * loop (PG 18 stores them as named pg_constraint entries with
	 * conislocal=true), and CHECK overrides do too.
	 */
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		List	   *dpcontext = NIL;

		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			char	   *defstr;

			if (att->attisdropped || att->attislocal || !att->atthasdef)
				continue;

			defstr = find_attrdef_text(rel, att->attnum, &dpcontext);
			if (defstr == NULL)
				continue;

			resetStringInfo(&buf);
			appendStringInfo(&buf,
							 "ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s;",
							 qualname,
							 quote_identifier(NameStr(att->attname)),
							 defstr);
			statements = lappend(statements, pstrdup(buf.data));
			pfree(defstr);
		}
	}

	/*
	 * Per-column attoptions — these can't be set inline in CREATE TABLE,
	 * so they come out as ALTER TABLE ... ALTER COLUMN col SET (...) after
	 * the table is created.  Typical use: n_distinct overrides for the
	 * planner.
	 */
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);

		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			HeapTuple	attTup;
			Datum		optDatum;
			bool		optIsnull;

			if (att->attisdropped)
				continue;

			attTup = SearchSysCache2(ATTNUM,
									 ObjectIdGetDatum(relid),
									 Int16GetDatum(att->attnum));
			if (!HeapTupleIsValid(attTup))
				continue;

			optDatum = SysCacheGetAttr(ATTNUM, attTup,
									   Anum_pg_attribute_attoptions, &optIsnull);
			if (!optIsnull)
			{
				resetStringInfo(&buf);
				appendStringInfo(&buf, "ALTER TABLE %s ALTER COLUMN %s SET (",
								 qualname,
								 quote_identifier(NameStr(att->attname)));
				get_reloptions(&buf, optDatum);
				appendStringInfoString(&buf, ");");
				statements = lappend(statements, pstrdup(buf.data));
			}
			ReleaseSysCache(attTup);
		}
	}

	/*
	 * Indexes — emit a CREATE INDEX for each non-constraint-backed index on
	 * the table.  Indexes that back PK/UNIQUE/EXCLUDE constraints are
	 * emitted by the constraint loop below as part of the ALTER TABLE ...
	 * ADD CONSTRAINT statement, which creates the index implicitly.
	 */
	if (include_indexes)
	{
		List	   *indexoids = RelationGetIndexList(rel);
		ListCell   *lc;

		foreach(lc, indexoids)
		{
			Oid			idxoid = lfirst_oid(lc);
			char	   *idxdef;

			if (OidIsValid(get_index_constraint(idxoid)))
				continue;

			idxdef = pg_get_indexdef_string(idxoid);
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%s;", idxdef);
			statements = lappend(statements, pstrdup(buf.data));
			pfree(idxdef);
		}
		list_free(indexoids);
	}

	/*
	 * Constraints — emit an ALTER TABLE ... ADD CONSTRAINT for each
	 * locally-defined constraint on the table.  Inherited constraints
	 * (conislocal=false) are produced by the parent's DDL and propagated
	 * automatically by INHERITS / PARTITION OF, so we skip them here.
	 */
	if (include_constraints)
	{
		Relation	conRel;
		SysScanDesc conScan;
		ScanKeyData conKey;
		HeapTuple	conTup;

		conRel = table_open(ConstraintRelationId, AccessShareLock);
		ScanKeyInit(&conKey,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		conScan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId,
									 true, NULL, 1, &conKey);

		while (HeapTupleIsValid(conTup = systable_getnext(conScan)))
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);
			char	   *condef;

			if (!con->conislocal)
				continue;
			/* CHECK constraints are emitted inline in the column list. */
			if (con->contype == CONSTRAINT_CHECK)
				continue;

			condef = pg_get_constraintdef_command(con->oid);
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%s;", condef);
			statements = lappend(statements, pstrdup(buf.data));
			pfree(condef);
		}
		systable_endscan(conScan);
		table_close(conRel, AccessShareLock);
	}

	/*
	 * Rules — emit a CREATE RULE for each cached rewrite rule on the
	 * relation.  Internal rules (such as the _RETURN rule on views) live on
	 * views/matviews and don't appear here because we already restricted
	 * relkind above.
	 */
	if (include_rules && rel->rd_rules != NULL)
	{
		for (int i = 0; i < rel->rd_rules->numLocks; i++)
		{
			Oid			ruleid = rel->rd_rules->rules[i]->ruleId;
			Datum		ruledef;
			char	   *ruledef_str;

			ruledef = OidFunctionCall1(F_PG_GET_RULEDEF_OID,
									   ObjectIdGetDatum(ruleid));
			ruledef_str = TextDatumGetCString(ruledef);
			resetStringInfo(&buf);
			appendStringInfoString(&buf, ruledef_str);
			statements = lappend(statements, pstrdup(buf.data));
			pfree(ruledef_str);
		}
	}

	/*
	 * Extended statistics — iterate pg_statistic_ext by stxrelid and emit
	 * pg_get_statisticsobjdef_string() for each.
	 */
	if (include_statistics)
	{
		Relation	statRel;
		SysScanDesc statScan;
		ScanKeyData statKey;
		HeapTuple	statTup;

		statRel = table_open(StatisticExtRelationId, AccessShareLock);
		ScanKeyInit(&statKey,
					Anum_pg_statistic_ext_stxrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		statScan = systable_beginscan(statRel, StatisticExtRelidIndexId,
									  true, NULL, 1, &statKey);

		while (HeapTupleIsValid(statTup = systable_getnext(statScan)))
		{
			Form_pg_statistic_ext stat = (Form_pg_statistic_ext) GETSTRUCT(statTup);
			char	   *statdef = pg_get_statisticsobjdef_string(stat->oid);

			resetStringInfo(&buf);
			appendStringInfo(&buf, "%s;", statdef);
			statements = lappend(statements, pstrdup(buf.data));
			pfree(statdef);
		}
		systable_endscan(statScan);
		table_close(statRel, AccessShareLock);
	}

	/*
	 * REPLICA IDENTITY — emit only when it differs from the default
	 * ('d' = use primary key).  This affects logical replication
	 * behavior, so round-trip fidelity matters.
	 */
	if (include_replica_identity &&
		rel->rd_rel->relreplident != REPLICA_IDENTITY_DEFAULT)
	{
		resetStringInfo(&buf);
		switch (rel->rd_rel->relreplident)
		{
			case REPLICA_IDENTITY_NOTHING:
				appendStringInfo(&buf, "ALTER TABLE %s REPLICA IDENTITY NOTHING;",
								 qualname);
				statements = lappend(statements, pstrdup(buf.data));
				break;
			case REPLICA_IDENTITY_FULL:
				appendStringInfo(&buf, "ALTER TABLE %s REPLICA IDENTITY FULL;",
								 qualname);
				statements = lappend(statements, pstrdup(buf.data));
				break;
			case REPLICA_IDENTITY_INDEX:
				{
					Oid			replidx = RelationGetReplicaIndex(rel);

					if (OidIsValid(replidx))
					{
						char	   *idxname = get_rel_name(replidx);

						if (idxname == NULL)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("replica identity index with OID %u does not exist",
											replidx),
									 errdetail("It may have been concurrently dropped.")));

						appendStringInfo(&buf,
										 "ALTER TABLE %s REPLICA IDENTITY USING INDEX %s;",
										 qualname,
										 quote_identifier(idxname));
						statements = lappend(statements, pstrdup(buf.data));
						pfree(idxname);
					}
				}
				break;
		}
	}

	/* ENABLE / FORCE ROW LEVEL SECURITY */
	if (include_rls && rel->rd_rel->relrowsecurity)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER TABLE %s ENABLE ROW LEVEL SECURITY;",
						 qualname);
		statements = lappend(statements, pstrdup(buf.data));
	}
	if (include_rls && rel->rd_rel->relforcerowsecurity)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER TABLE %s FORCE ROW LEVEL SECURITY;",
						 qualname);
		statements = lappend(statements, pstrdup(buf.data));
	}

	/*
	 * Triggers — scaffolding only.  The standalone pg_get_trigger_ddl()
	 * function (Phil's re-roll) is the intended emission path; once it
	 * lands the body of this loop becomes a single call into it.  The
	 * scan structure, lock acquisition, and tgisinternal filter (which
	 * skips FK-backing and other system-generated triggers) are settled
	 * here so that change stays minimal.
	 */
	if (include_triggers)
	{
		Relation	trigRel;
		SysScanDesc trigScan;
		ScanKeyData trigKey;
		HeapTuple	trigTup;

		trigRel = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyInit(&trigKey,
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		trigScan = systable_beginscan(trigRel, TriggerRelidNameIndexId,
									  true, NULL, 1, &trigKey);
		while (HeapTupleIsValid(trigTup = systable_getnext(trigScan)))
		{
			Form_pg_trigger trg = (Form_pg_trigger) GETSTRUCT(trigTup);

			if (trg->tgisinternal)
				continue;

			/* TODO: append pg_get_trigger_ddl(trg->oid) output here. */
			(void) trg;
		}
		systable_endscan(trigScan);
		table_close(trigRel, AccessShareLock);
	}

	/*
	 * Row-level security policies — scaffolding only.  Once
	 * pg_get_policy_ddl() (already-submitted patch) lands, the body of
	 * this loop becomes a per-policy call into it.  The ENABLE/FORCE
	 * ROW LEVEL SECURITY toggles above are the companion catalog flags
	 * and are already emitted independently of policies.
	 */
	if (include_policies)
	{
		Relation	polRel;
		SysScanDesc polScan;
		ScanKeyData polKey;
		HeapTuple	polTup;

		polRel = table_open(PolicyRelationId, AccessShareLock);
		ScanKeyInit(&polKey,
					Anum_pg_policy_polrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		polScan = systable_beginscan(polRel, PolicyPolrelidPolnameIndexId,
									 true, NULL, 1, &polKey);
		while (HeapTupleIsValid(polTup = systable_getnext(polScan)))
		{
			Form_pg_policy pol = (Form_pg_policy) GETSTRUCT(polTup);

			/* TODO: append pg_get_policy_ddl(relid, polname) output here. */
			(void) pol;
		}
		systable_endscan(polScan);
		table_close(polRel, AccessShareLock);
	}

	/*
	 * Partition children — when include_partition is true and this relation
	 * is a partitioned-table parent, recursively emit the DDL for each
	 * direct partition child.  Each child's own DDL handles further levels
	 * of sub-partitioning through the same recursion.
	 */
	if (include_partition && relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *children = find_inheritance_children(relid, AccessShareLock);
		ListCell   *lc;

		foreach(lc, children)
		{
			Oid			childoid = lfirst_oid(lc);
			List	   *childstmts;

			childstmts = pg_get_table_ddl_internal(childoid, pretty,
												   no_owner, no_tablespace,
												   include_indexes,
												   include_constraints,
												   include_rules,
												   include_statistics,
												   include_triggers,
												   include_policies,
												   include_rls,
												   include_replica_identity,
												   include_partition);
			statements = list_concat(statements, childstmts);
		}
		list_free(children);
	}

	table_close(rel, AccessShareLock);
	pfree(buf.data);
	pfree(qualname);

	return statements;
}

/*
 * pg_get_table_ddl
 *		Return DDL to recreate a table as a set of text rows.
 */
Datum
pg_get_table_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			relid;
		DdlOption	opts[] = {
			{"pretty", DDL_OPT_BOOL},
			{"owner", DDL_OPT_BOOL},
			{"tablespace", DDL_OPT_BOOL},
			{"includes_indexes", DDL_OPT_BOOL},
			{"includes_constraints", DDL_OPT_BOOL},
			{"includes_rules", DDL_OPT_BOOL},
			{"includes_statistics", DDL_OPT_BOOL},
			{"includes_triggers", DDL_OPT_BOOL},
			{"includes_policies", DDL_OPT_BOOL},
			{"includes_rls", DDL_OPT_BOOL},
			{"includes_replica_identity", DDL_OPT_BOOL},
			{"includes_partition", DDL_OPT_BOOL},
		};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		relid = PG_GETARG_OID(0);
		parse_ddl_options(fcinfo, 1, opts, lengthof(opts));

		statements = pg_get_table_ddl_internal(relid,
											   opts[0].isset && opts[0].boolval,
											   opts[1].isset && !opts[1].boolval,
											   opts[2].isset && !opts[2].boolval,
											   !opts[3].isset || opts[3].boolval,
											   !opts[4].isset || opts[4].boolval,
											   !opts[5].isset || opts[5].boolval,
											   !opts[6].isset || opts[6].boolval,
											   !opts[7].isset || opts[7].boolval,
											   !opts[8].isset || opts[8].boolval,
											   !opts[9].isset || opts[9].boolval,
											   !opts[10].isset || opts[10].boolval,
											   opts[11].isset && opts[11].boolval);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}
