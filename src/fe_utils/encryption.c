/*-------------------------------------------------------------------------
 *
 * encryption.c
 *	  Front-end code to handle keys for full cluster encryption. The actual
 *	  encryption is not performed here.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/fe_utils/encryption.c
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>

#include "postgres_fe.h"

#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/encryption.h"
#include "storage/encryption.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/pqcomm.h"

char	   *encryption_key_command = NULL;

/*
 * Run the command that is supposed to generate encryption key and store it
 * where encryption_key points to.
 */
void
run_encryption_key_command(unsigned char *encryption_key)
{
	FILE	   *fp;
	char	   *buf;
	int		read_len, i, c;

	Assert(encryption_key_command != NULL &&
		   strlen(encryption_key_command) > 0);

	fp = popen(encryption_key_command, "r");
	if (fp == NULL)
	{
		pg_log_fatal("Failed to execute \"%s\"", encryption_key_command);
		exit(EXIT_FAILURE);
	}

	buf = (char *) palloc(ENCRYPTION_KEY_CHARS);

	/*
	 * Read the key. This is very similar to backend's read_encryption_key()
	 * but there seems to be no straightforward way to call the function from
	 * here.
	 */
	read_len = 0;
	while ((c = fgetc(fp)) != EOF && c != '\n')
	{
		if (read_len >= ENCRYPTION_KEY_CHARS)
		{
			pg_log_fatal("Encryption key is too long");
			exit(EXIT_FAILURE);
		}

		buf[read_len++] = c;
	}

	if (read_len < ENCRYPTION_KEY_CHARS)
	{
		pg_log_fatal("Encryption key is too short");
		exit(EXIT_FAILURE);
	}

	/* Turn the hexadecimal representation into an array of bytes. */
	for (i = 0; i < ENCRYPTION_KEY_LENGTH; i++)
	{
		if (sscanf(buf + 2 * i, "%2hhx", encryption_key + i) == 0)
		{
			pg_log_fatal("Invalid character in encryption key at position %d",
						 2 * i);
			exit(EXIT_FAILURE);
		}
	}

	pfree(buf);
	pclose(fp);
}
