/*-------------------------------------------------------------------------
 *
 * test_getopt_long.c
 *    Test program for the src/port implementation of getopt_long()
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/modules/test_getopt_long/test_getopt_long.c
 *
 * Usage: test_getopt_long OPTSTRING LONGOPTS [ARG ...]
 *
 * LONGOPTS is a comma separated list of long option names, each optionally
 * followed by ":" (required argument) or "::" (optional argument), and
 * optionally prefixed with "*" to make getopt_long() set a flag variable
 * instead of returning a value.  The remaining ARGs are parsed with
 * getopt_long() and each return is printed on its own line:
 *
 *   -x            short option x
 *   -x=VALUE      short option x with argument VALUE
 *   --name        long option name
 *   --name=VALUE  long option name with argument VALUE
 *   flag:--name   long option name, delivered via its flag pointer
 *   ?             BADCH (unknown option or missing argument)
 *   :             BADARG (missing argument, optstring starts with ':')
 *
 * After getopt_long() returns -1, a final line "--" lists the remaining
 * (non-option) arguments, space separated.
 *
 * src/port/getopt_long.c is compiled into this program directly so the
 * port implementation is tested even on platforms where libpgport would
 * normally use the system's getopt_long().  test_getopt_long_system is the
 * same program linked without it, for comparison against the system one.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "getopt_long.h"

#define MAX_LONGOPTS 32

/* long options without a flag return this plus their index */
#define LONGOPT_BASE 1000

static struct option longopts[MAX_LONGOPTS + 1];
static char *longnames[MAX_LONGOPTS];
static int	flagval;

static void
parse_longopts(char *spec)
{
	int			n = 0;
	char	   *tok;

	for (tok = strtok(spec, ","); tok != NULL; tok = strtok(NULL, ","))
	{
		struct option *opt = &longopts[n];
		char	   *colon;

		if (n >= MAX_LONGOPTS)
		{
			fprintf(stderr, "too many long options\n");
			exit(1);
		}

		if (tok[0] == '*')
		{
			tok++;
			opt->flag = &flagval;
			opt->val = n + 1;
		}
		else
		{
			opt->flag = NULL;
			opt->val = LONGOPT_BASE + n;
		}

		colon = strchr(tok, ':');
		if (colon == NULL)
			opt->has_arg = no_argument;
		else if (colon[1] == ':')
			opt->has_arg = optional_argument;
		else
			opt->has_arg = required_argument;
		if (colon != NULL)
			*colon = '\0';

		longnames[n] = tok;
		opt->name = tok;
		n++;
	}

	longopts[n].name = NULL;
}

int
main(int argc, char **argv)
{
	const char *optstring;
	char	  **args;
	int			nargs;
	int			c;

	if (argc < 3)
	{
		fprintf(stderr, "Usage: %s OPTSTRING LONGOPTS [ARG ...]\n", argv[0]);
		exit(1);
	}

	optstring = argv[1];
	parse_longopts(argv[2]);

	/* build the argv that getopt_long() will see, and may reorder */
	nargs = argc - 2;
	args = palloc((nargs + 1) * sizeof(char *));
	args[0] = argv[0];
	for (int i = 1; i < nargs; i++)
		args[i] = argv[i + 2];
	args[nargs] = NULL;

	while ((c = getopt_long(nargs, args, optstring, longopts, NULL)) != -1)
	{
		if (c == 0)
		{
			printf("flag:--%s\n", longnames[flagval - 1]);
			continue;
		}

		if (c >= LONGOPT_BASE)
			printf("--%s", longnames[c - LONGOPT_BASE]);
		else if (c == '?' || c == ':')
			printf("%c", c);
		else
			printf("-%c", c);

		if (optarg != NULL)
			printf("=%s", optarg);
		printf("\n");
	}

	printf("--");
	for (int i = optind; i < nargs; i++)
		printf(" %s", args[i]);
	printf("\n");

	return 0;
}
