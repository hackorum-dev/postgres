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
 *
 * TODO Replace pattern (e.g. %D) in the command with data directory so that
 * DBA knows for which cluster he enters the password. That should also make
 * the use of pg_keytool in the command easier.
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

/*
 * Send the contents of encryption_key in the form of special startup packet
 * to a server that is being started.
 *
 * Returns true if we could send the message and false if not, however even
 * success does not guarantee that server started up - caller should
 * eventually test server connection himself.
 *
 * TODO Windows stuff?
 */
bool
send_key_to_postmaster(const char *host, const char *port,
					   const unsigned char *encryption_Key)
{
	const char **keywords = pg_malloc0(3 * sizeof(*keywords));
	const char **values = pg_malloc0(3 * sizeof(*values));
	int	i;
	PGconn *conn = NULL;
	int	sock = -1;
	EncryptionKeyMsg	message;
	int	msg_size, packet_size;
	char	*packet = NULL;
	bool	res = true;

/* How many seconds we can wait for the postmaster to receive the key. */
#define SEND_ENCRYPT_KEY_TIMEOUT	60

	if (host)
	{
		keywords[0] = "host";
		values[0] = host;
	}
	keywords[1] = "port";
	values[1] = port;

	/* Compose the message. */
	message.encryptionKeyCode = pg_hton32(ENCRYPTION_KEY_MSG_CODE);
	message.version = 1;
	memcpy(message.data, encryption_key, ENCRYPTION_KEY_LENGTH);
	msg_size = offsetof(EncryptionKeyMsg, data) + ENCRYPTION_KEY_LENGTH;

	packet_size = msg_size + 4;
	packet = (char *) palloc(packet_size);
	*((int32 *) packet) = pg_hton32(packet_size);
	memcpy(packet + 4, &message, msg_size);

	/*
	 * Although we don't expect the server to accept regular libpq messages,
	 * we try to get at least a valid socket.
	 */
	for (i = 0; i < SEND_ENCRYPT_KEY_TIMEOUT + 1; i++)
	{
		if (i > 0)
			/* Sleep for 1 second. */
			pg_usleep(1000000L);

		if (conn)
		{
			PQfinish(conn);
			conn = NULL;
		}

		conn = PQconnectStartParams(keywords, values, false);
		if (conn == NULL)
			continue;

		sock = PQsocket(conn);
		/* Cannot send the key if there's no valid socket. */
		if (sock == -1)
			continue;

		/* Non-blocking write would only make this simple case tricky. */
		if (!pg_set_block(sock))
			continue;

	retry:
		/*
		 * Send the packet. Here we need to use low level API because the server
		 * does is not fully up so libpq cannot be used properly.
		 */
		if (send(sock, (char *) packet, packet_size, 0) != packet_size)
		{
			if (SOCK_ERRNO == EINTR)
				/* Interrupted system call - we'll just try again */
				goto retry;
			continue;
		}
		else
			/* Success */
			break;
	}

	pg_free(keywords);
	pg_free(values);
	if (conn)
		PQfinish(conn);
	pfree(packet);

	return res;
}
