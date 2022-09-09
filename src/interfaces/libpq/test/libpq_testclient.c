/*
 * libpq_testclient.c
 *		A test program for the libpq public API
 *
 * Copyright (c) 2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/interfaces/libpq/test/libpq_testclient.c
 */

#include "postgres_fe.h"

#include "libpq-fe.h"

static void
print_ssl_library()
{
	const char *lib = PQsslAttribute(NULL, "library");

	if (!lib)
		fputs("SSL is not enabled\n", stderr);
	else
		printf("%s\n", lib);
}

int
main(int argc, char *argv[])
{
	if ((argc > 1) && !strcmp(argv[1], "--ssl"))
	{
		print_ssl_library();
		return 0;
	}

	printf("currently only --ssl is supported\n");
	return 1;
}
