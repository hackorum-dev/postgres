/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/copy.c
 */
#include "postgres_fe.h"

#include "common.h"
#include "common/logging.h"
#include "create_function.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "settings.h"
#include "stringutils.h"

struct create_function_options
{
	char	   *from_file;
	char	   *after_from;
};

static void
free_create_function_options(struct create_function_options *ptr)
{
	if (!ptr)
		return;
	free(ptr->from_file);
	free(ptr->after_from);
	free(ptr);
}

static struct create_function_options *
parse_slash_create_function(const char *args)
{
	struct create_function_options *result;
	char	   *token;
	const char *whitespace = " \t\n\r";

	if (!args)
	{
		pg_log_error("\\create_function: arguments required");
		return NULL;
	}

	result = pg_malloc0(sizeof(struct create_function_options));

	token = strtokx(args, whitespace, NULL, NULL,
					0, false, false, pset.encoding);

	if (pg_strcasecmp(token, "from") != 0)
		goto error;

	token = strtokx(NULL, whitespace, NULL, NULL,
					0, false, false, pset.encoding);

	if(!token)
		goto error;

	result->from_file = pg_strdup(token);

	token = strtokx(NULL, "", ";", NULL,
						0, false, false, pset.encoding);

	if (!token)
		goto error;

	result->after_from = pg_strdup(token);

	return result;

error:
	if (token)
		pg_log_error("\\create_function: parse error at \"%s\"", token);
	else
		pg_log_error("\\create_function: parse error at end of line");

	free_create_function_options(result);

	return NULL;
}

bool
do_create_function(const char *args)
{
	PQExpBufferData query;
	struct create_function_options *options = parse_slash_create_function(args);
	FILE	   *func_file;
	bool		success;

	if (!options)
		return false;
	else{
		char buf[255];

		initPQExpBuffer(&query);
		printfPQExpBuffer(&query, "CREATE OR REPLACE FUNCTION ");
		appendPQExpBufferStr(&query, options->after_from);
		appendPQExpBufferStr(&query, " AS $___$");

		expand_tilde(&(options->from_file));
		canonicalize_path(options->from_file);
		func_file = fopen(options->from_file, PG_BINARY_R);

		if (!func_file)
		{
			pg_log_error("%s: %m", options->from_file);
			free_create_function_options(options);
			return false;
		}

		while(fgets(buf, sizeof(buf), func_file) != NULL)
			appendPQExpBufferStr(&query, buf);

		fclose(func_file);

		appendPQExpBufferStr(&query, " $___$;");

		success = SendQuery(query.data);

		termPQExpBuffer(&query);

		free_create_function_options(options);

		return success;
	}
}
