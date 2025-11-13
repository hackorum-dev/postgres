/*-------------------------------------------------------------------------
 *
 * pg_tablespace.c
 *	  routines to support manipulation of the pg_tablespace relation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/htup_details.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/*
 * get_tablespace_location
 *		Get a tablespace's location as a C-string, by its OID
 */
char *
get_tablespace_location(Oid tablespaceOid)
{
	char		sourcepath[MAXPGPATH];
	char		targetpath[MAXPGPATH];
	int			rllen;
	struct stat st;

	/*
	 * It's useful to apply this to pg_class.reltablespace, wherein zero means
	 * "the database's default tablespace".  So, rather than throwing an error
	 * for zero, we choose to assume that's what is meant.
	 */
	if (tablespaceOid == InvalidOid)
		tablespaceOid = MyDatabaseTableSpace;

	/*
	 * Return empty string for the cluster's default tablespaces
	 */
	if (tablespaceOid == DEFAULTTABLESPACE_OID ||
		tablespaceOid == GLOBALTABLESPACE_OID)
		return pstrdup("");

	/*
	 * Find the location of the tablespace by reading the symbolic link that
	 * is in pg_tblspc/<oid>.
	 */
	snprintf(sourcepath, sizeof(sourcepath), "%s/%u", PG_TBLSPC_DIR, tablespaceOid);

	/*
	 * Before reading the link, check if the source path is a link or a
	 * junction point.  Note that a directory is possible for a tablespace
	 * created with allow_in_place_tablespaces enabled.  If a directory is
	 * found, a relative path to the data directory is returned.
	 */
	if (lstat(sourcepath, &st) < 0)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not stat file \"%s\": %m",
					   sourcepath));

	if (!S_ISLNK(st.st_mode))
		return pstrdup(sourcepath);

	/*
	 * In presence of a link or a junction point, return the path pointed to.
	 */
	rllen = readlink(sourcepath, targetpath, sizeof(targetpath));
	if (rllen < 0)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not read symbolic link \"%s\": %m",
					   sourcepath));
	if (rllen >= sizeof(targetpath))
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("symbolic link \"%s\" target is too long",
					   sourcepath));
	targetpath[rllen] = '\0';

	return pstrdup(targetpath);
}

/*
 * build_tablespace_ddl_string - Build CREATE TABLESPACE statement as a
 * C-string for a tablespace from its OID.
 */
char *
build_tablespace_ddl_string(const Oid tspaceoid)
{
	char	   *path;
	char	   *spcowner;
	bool		isNull;
	Oid			tspowneroid;
	Datum		datum;
	HeapTuple	tuple;
	StringInfoData buf;
	Form_pg_tablespace tspForm;

	/* Look up the tablespace in pg_tablespace */
	tuple = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(tspaceoid));

	/* Confirm if tablespace OID was valid */
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with oid %u does not exist",
						tspaceoid)));

	/* Get tablespace's details from its tuple */
	tspForm = (Form_pg_tablespace) GETSTRUCT(tuple);

	initStringInfo(&buf);

	/* Start building the CREATE TABLESPACE statement */
	appendStringInfo(&buf, "CREATE TABLESPACE %s",
					 quote_identifier(NameStr(tspForm->spcname)));

	/* Get the OID of the owner of the tablespace name */
	tspowneroid = tspForm->spcowner;

	/* Add OWNER clause, if the owner is not the current user */
	if (GetUserId() != tspowneroid)
	{
		/* Get the owner name */
		spcowner = GetUserNameFromId(tspowneroid, false);

		appendStringInfo(&buf, " OWNER %s",
						 quote_identifier(spcowner));
		pfree(spcowner);
	}

	/* Find tablespace directory path */
	path = get_tablespace_location(tspaceoid);

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
			appendStringInfoString(&buf, " LOCATION ''");
		else
			appendStringInfo(&buf, " LOCATION %s", quote_literal_cstr(path));
	}
	/* Done with path */
	pfree(path);

	/* Get tablespace's options datum from the tuple */
	datum = SysCacheGetAttr(TABLESPACEOID,
							tuple,
							Anum_pg_tablespace_spcoptions,
							&isNull);

	if (!isNull)
	{
		ArrayType  *optarray;
		Datum	   *optdatums;
		int			optcount;
		int			i;

		optarray = DatumGetArrayTypeP(datum);

		deconstruct_array_builtin(optarray, TEXTOID,
								  &optdatums, NULL, &optcount);

		Assert(optcount);

		/* Start WITH clause */
		appendStringInfoString(&buf, " WITH (");

		for (i = 0; i < (optcount - 1); i++)	/* Skipping last option */
		{
			/* Add the options in WITH clause */
			appendStringInfo(&buf, "%s, ", TextDatumGetCString(optdatums[i]));
		}

		/* Adding the last remaining option */
		appendStringInfoString(&buf, TextDatumGetCString(optdatums[i]));
		/* Closing WITH clause */
		appendStringInfoChar(&buf, ')');
		/* Cleanup the datums found */
		pfree(optdatums);
	}

	ReleaseSysCache(tuple);

	/* Finally add semicolon to the statement */
	appendStringInfoChar(&buf, ';');

	return buf.data;
}
