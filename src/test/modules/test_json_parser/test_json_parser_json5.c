/*-------------------------------------------------------------------------
 *
 * test_json_parser_json5.c
 *    Test program for JSON5 mode of the recursive descent JSON parser
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/modules/test_json_parser/test_json_parser_json5.c
 *
 * This program parses its input with the recursive descent (non-incremental)
 * JSON parser, pg_parse_json(). Unless "-p" (plain) is given, JSON5 mode is
 * enabled, so that comments, trailing commas, single-quoted strings and
 * unquoted object keys are all accepted in addition to standard JSON.
 *
 * If the -s flag is given, the program does semantic processing, mirroring
 * back the input as standard JSON (albeit with white space changes). This
 * can be used to confirm that JSON5-only syntax is interpreted correctly,
 * not just that it's accepted.
 *
 * The argument specifies the file containing the JSON input.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>

#include "common/jsonapi.h"
#include "common/logging.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "pg_getopt.h"

#define BUFSIZE 6000

typedef struct DoState
{
	bool		elem_is_first;
	StringInfo	buf;
} DoState;

static void usage(const char *progname);
static void escape_json(StringInfo buf, const char *str);

/* semantic action functions for parser */
static JsonParseErrorType do_object_start(void *state);
static JsonParseErrorType do_object_end(void *state);
static JsonParseErrorType do_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType do_array_start(void *state);
static JsonParseErrorType do_array_end(void *state);
static JsonParseErrorType do_array_element_start(void *state, bool isnull);
static JsonParseErrorType do_scalar(void *state, char *token, JsonTokenType tokentype);

static JsonSemAction sem = {
	.object_start = do_object_start,
	.object_end = do_object_end,
	.object_field_start = do_object_field_start,
	.array_start = do_array_start,
	.array_end = do_array_end,
	.array_element_start = do_array_element_start,
	.scalar = do_scalar
};

int
main(int argc, char **argv)
{
	char		buff[BUFSIZE];
	FILE	   *json_file;
	JsonParseErrorType result;
	JsonLexContext *lex;
	StringInfoData json;
	int			n_read;
	bool		json5 = true;
	bool		need_strings = false;
	const JsonSemAction *testsem = &nullSemAction;
	char	   *testfile;
	int			c;
	DoState		state;

	pg_logging_init(argv[0]);

	while ((c = getopt(argc, argv, "ps")) != -1)
	{
		switch (c)
		{
			case 'p':			/* plain JSON, not JSON5 */
				json5 = false;
				break;
			case 's':			/* do semantic processing */
				testsem = &sem;
				need_strings = true;
				break;
		}
	}

	if (optind < argc)
	{
		testfile = argv[optind];
		optind++;
	}
	else
	{
		usage(argv[0]);
		exit(1);
	}

	initStringInfo(&json);

	if ((json_file = fopen(testfile, PG_BINARY_R)) == NULL)
		pg_fatal("error opening input: %m");

	while ((n_read = fread(buff, 1, BUFSIZE, json_file)) > 0)
		appendBinaryStringInfo(&json, buff, n_read);
	fclose(json_file);

	lex = makeJsonLexContextCstringLen(NULL, json.data, json.len,
									   PG_UTF8, need_strings);
	if (json5 && setJsonLexContextJSON5(lex, true) != JSON_SUCCESS)
		pg_fatal("could not enable JSON5 mode");

	if (testsem == &sem)
	{
		state.elem_is_first = true;
		state.buf = makeStringInfo();
		sem.semstate = &state;
	}

	result = pg_parse_json(lex, testsem);

	if (result != JSON_SUCCESS)
	{
		fprintf(stderr, "%s\n", json_errdetail(result, lex));
		freeJsonLexContext(lex);
		free(json.data);
		return 1;
	}

	if (!need_strings)
		printf("SUCCESS!\n");

	freeJsonLexContext(lex);
	free(json.data);

	return 0;
}

/*
 * The semantic routines here essentially just output the same json, except
 * for white space and, in JSON5 mode, resolving any JSON5-only syntax into
 * plain JSON. The result should be able to be fed to any JSON processor
 * such as jq for validation.
 */

static JsonParseErrorType
do_object_start(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("{\n");
	_state->elem_is_first = true;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_object_end(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("\n}\n");
	_state->elem_is_first = false;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_object_field_start(void *state, char *fname, bool isnull)
{
	DoState    *_state = (DoState *) state;

	if (!_state->elem_is_first)
		printf(",\n");
	resetStringInfo(_state->buf);
	escape_json(_state->buf, fname);
	printf("%s: ", _state->buf->data);
	_state->elem_is_first = false;
	free(fname);

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_array_start(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("[\n");
	_state->elem_is_first = true;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_array_end(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("\n]\n");
	_state->elem_is_first = false;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_array_element_start(void *state, bool isnull)
{
	DoState    *_state = (DoState *) state;

	if (!_state->elem_is_first)
		printf(",\n");
	_state->elem_is_first = false;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_scalar(void *state, char *token, JsonTokenType tokentype)
{
	DoState    *_state = (DoState *) state;

	if (tokentype == JSON_TOKEN_STRING)
	{
		resetStringInfo(_state->buf);
		escape_json(_state->buf, token);
		printf("%s", _state->buf->data);
	}
	else
		printf("%s", token);

	free(token);

	return JSON_SUCCESS;
}

/* copied from backend code */
static void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(buf, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(buf, *p);
				break;
		}
	}
	appendStringInfoCharMacro(buf, '"');
}

static void
usage(const char *progname)
{
	fprintf(stderr,
			"Usage: %s [-p] [-s] filename\n"
			"  -p  parse as plain JSON, not JSON5\n"
			"  -s  do semantic processing\n",
			progname);
}
