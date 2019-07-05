/*-------------------------------------------------------------------------
 *
 * encryption.h
 *	  Full database encryption support
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/encryption.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENCRYPTION_H
#define ENCRYPTION_H

#include "access/xlogdefs.h"
#include "miscadmin.h"
#include "storage/block.h"
#include "storage/relfilenode.h"
#include "port/pg_crc32c.h"

/*
 * Common error message issued when particular code path cannot be executed
 * due to absence of the OpenSSL library.
 */
#define ENCRYPTION_NOT_SUPPORTED_MSG \
	"compile postgres with --with-openssl to use encryption."

/*
 * Full database encryption key.
 *
 * The key of EVP_aes_256_cbc() cipher is 256 bits long.
 */
#define	ENCRYPTION_KEY_LENGTH	32
/* Key length in characters (two characters per hexadecimal digit) */
#define ENCRYPTION_KEY_CHARS	(ENCRYPTION_KEY_LENGTH * 2)

/*
 * Cipher used to encrypt data.
 *
 * Due to very specific requirements, the ciphers are not likely to change,
 * but we should be somewhat flexible.
 *
 * XXX If we have more than one cipher someday, have pg_controldata report the
 * cipher kind (in textual form) instead of merely saying "on".
 */
typedef enum CipherKind
{
	/* The cluster is not encrypted. */
	PG_CIPHER_NONE = 0,

	/*
	 * AES (Rijndael) in CBC mode of operation as block cipher, and in CTR
	 * mode as stream cipher. Key length is always 256 bits.
	 */
	PG_CIPHER_AES_BLOCK_CBC_256_STREAM_CTR_256
}			CipherKind;

/* Key to encrypt / decrypt data. */
extern unsigned char encryption_key[];

/*
 * The encrypted data is a series of blocks of size
 * ENCRYPTION_BLOCK. Currently we use the EVP_aes_256_xts implementation. Make
 * sure the following constants match if adopting another algorithm.
 */
#define ENCRYPTION_BLOCK 16

#define TWEAK_SIZE 16

/* Is the cluster encrypted? */
extern PGDLLIMPORT bool data_encrypted;

/*
 * Number of bytes reserved to store encryption sample in ControlFileData.
 */
#define ENCRYPTION_SAMPLE_SIZE 16

#ifndef FRONTEND
/* Copy of the same field of ControlFileData. */
extern char encryption_verification[];
#endif							/* FRONTEND */

/* Do we have encryption_key and the encryption library initialized? */
extern bool	encryption_setup_done;

#ifndef FRONTEND
typedef int (*read_encryption_key_cb) (void);
extern void read_encryption_key(read_encryption_key_cb read_char);
#endif							/* FRONTEND */

extern void setup_encryption(void);
extern void sample_encryption(char *buf);
extern void encrypt_block(const char *input, char *output, Size size,
							char *tweak, bool stream);
extern void encryption_error(bool fatal, char *message);

#endif							/* ENCRYPTION_H */
