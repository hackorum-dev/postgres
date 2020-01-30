/*
 *	pg_test_json.c
 *		tests validity of json strings against parser implementation.
 */

#include "postgres_fe.h"

#include "common/jsonapi.h"
#include "libpq-fe.h"

static const char *progname;

static void parse_json(const char *str);

int
main(int argc, char *argv[])
{
	int			argidx;

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			printf(_("Usage: %s jsonstr [, ...]\n"), progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_test_json (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	for (argidx = 1; argidx < argc; argidx++)
		parse_json(argv[argidx]);

	return 0;
}

static void
parse_json(const char *str)
{
	char *json;
	JsonLexContext *lex;
	int client_encoding;
	JsonParseErrorType parse_result;

	client_encoding = PQenv2encoding();

	json = strdup(str);
	lex = makeJsonLexContextCstringLen(json, strlen(json), client_encoding, true /* need_escapes */);
	parse_result = pg_parse_json(lex, &nullSemAction);
	if (JSON_SUCCESS == parse_result)
		fprintf(stdout, _("VALID\n"));
	else
	{
		const char *errstr = json_errdetail(parse_result, lex);
		fprintf(stdout, _("%s\n"), errstr);
	}
}
