/*
 * faultinjector.c
 *
 * SQL interface to inject a pre-defined fault in backend code.
 */
#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"

#include "libpq-fe.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"

PG_MODULE_MAGIC;

extern Datum inject_fault(PG_FUNCTION_ARGS);
extern Datum inject_fault_remote(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(inject_fault);
PG_FUNCTION_INFO_V1(inject_fault_remote);

/*
 * SQL UDF to inject a fault by associating an action against it.  See
 * the accompanying README for more details.
 */
Datum
inject_fault(PG_FUNCTION_ARGS)
{
	char	   *faultName = TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *type = TextDatumGetCString(PG_GETARG_DATUM(1));
	char	   *databaseName = TextDatumGetCString(PG_GETARG_DATUM(2));
	char	   *tableName = TextDatumGetCString(PG_GETARG_DATUM(3));
	int			startOccurrence = PG_GETARG_INT32(4);
	int			endOccurrence = PG_GETARG_INT32(5);
	int			extraArg = PG_GETARG_INT32(6);
	char	   *response;

	response = InjectFault(
						   faultName, type, databaseName, tableName,
						   startOccurrence, endOccurrence, extraArg);
	if (!response)
		elog(ERROR, "failed to inject fault");
	if (strncmp(response, "Success:", strlen("Success:")) != 0)
		elog(ERROR, "%s", response);
	PG_RETURN_TEXT_P(cstring_to_text(response));
}

Datum
inject_fault_remote(PG_FUNCTION_ARGS)
{
	char	   *faultName = TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *type = TextDatumGetCString(PG_GETARG_DATUM(1));
	char	   *databaseName = TextDatumGetCString(PG_GETARG_DATUM(2));
	char	   *tableName = TextDatumGetCString(PG_GETARG_DATUM(3));
	int			startOccurrence = PG_GETARG_INT32(4);
	int			endOccurrence = PG_GETARG_INT32(5);
	int			extraArg = PG_GETARG_INT32(6);
	char	   *hostname = TextDatumGetCString(PG_GETARG_DATUM(7));
	int			port = PG_GETARG_INT32(8);
	char	   *response;
	char	    conninfo[1024];
	char	    msg[1024];
	PGconn	   *conn;
	PGresult   *res;

	/* Set special connection option "fault=true" */
	snprintf(conninfo, 1024, "host=%s port=%d fault=true", hostname, port);
	conn = PQconnectdb(conninfo);
	if (PQstatus(conn) != CONNECTION_OK)
		elog(ERROR, "connection to %s:%d failed: %s",
			 hostname, port, PQerrorMessage(conn));

	/*
	 * If dbname or tablename is not specified, send '#' instead.  This allows
	 * sscanf to be used on the receiving end to parse the message.
	 */
	if (!databaseName || databaseName[0] == '\0')
		databaseName = "#";
	if (!tableName || tableName[0] == '\0')
		tableName = "#";
	snprintf(msg, 1024, "faultname=%s type=%s db=%s table=%s "
			 "start=%d end=%d extra=%d",
			 faultName, type,
			 databaseName,
			 tableName,
			 startOccurrence,
			 endOccurrence,
			 extraArg);

	res = PQexec(conn, msg);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		elog(ERROR, "failed to inject fault: %s", PQerrorMessage(conn));

	if (PQntuples(res) != 1)
	{
		PQclear(res);
		PQfinish(conn);
		elog(ERROR, "invalid response from %s:%d", hostname, port);
	}

	response = PQgetvalue(res, 0, Anum_fault_message_response_status);
	if (strncmp(response, "Success:",  strlen("Success:")) != 0)
	{
		PQclear(res);
		PQfinish(conn);
		elog(ERROR, "%s", response);
	}

	PQclear(res);
	PQfinish(conn);

	if (!response)
		elog(ERROR, "failed to inject fault");
	if (strncmp(response, "Success:", strlen("Success:")) != 0)
		elog(ERROR, "%s", response);
	PG_RETURN_TEXT_P(cstring_to_text(response));
}
