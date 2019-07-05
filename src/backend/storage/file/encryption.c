/*-------------------------------------------------------------------------
 *
 * encryption.c
 *	  This code handles encryption and decryption of data.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * See src/backend/storage/file/README.encryption for explanation of the
 * design.
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/encryption.c
 *
 * NOTES
 *		This file is compiled as both front-end and backend code, so the
 *		FRONTEND macro must be used to distinguish the case if we need to
 *		report error or if server-defined variable / function seems useful.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "common/fe_memutils.h"
#include "common/sha2.h"
#include "common/string.h"
#include "catalog/pg_control.h"
#include "storage/bufpage.h"
#include "storage/encryption.h"

#ifndef FRONTEND
#include "port.h"
#include "storage/shmem.h"
#include "storage/fd.h"
#include "utils/memutils.h"
#endif							/* FRONTEND */

#ifdef USE_ENCRYPTION
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>

EVP_CIPHER_CTX *ctx_encrypt,
		   *ctx_decrypt,
		   *ctx_encrypt_stream,
		   *ctx_decrypt_stream;
#endif							/* USE_ENCRYPTION */

unsigned char encryption_key[ENCRYPTION_KEY_LENGTH];

bool		data_encrypted = false;

char encryption_verification[ENCRYPTION_SAMPLE_SIZE];

bool	encryption_setup_done = false;

#ifdef USE_ENCRYPTION
static void init_encryption_context(EVP_CIPHER_CTX **ctx_p, bool stream);
static void evp_error(void);
#endif							/* USE_ENCRYPTION */

#ifndef FRONTEND
/*
 * Read encryption key in hexadecimal form from stdin and store it in
 * encryption_key variable.
 */
void
read_encryption_key(read_encryption_key_cb read_char)
{
	char	*buf;
	int		read_len, i, c;

	buf = (char *) palloc(ENCRYPTION_KEY_CHARS);

	read_len = 0;
	while ((c = (*read_char)()) != EOF && c != '\n')
	{
		if (read_len >= ENCRYPTION_KEY_CHARS)
			ereport(FATAL, (errmsg("Encryption key is too long")));

		buf[read_len++] = c;
	}

	if (read_len < ENCRYPTION_KEY_CHARS)
		ereport(FATAL, (errmsg("Encryption key is too short")));

	/* Turn the hexadecimal representation into an array of bytes. */
	for (i = 0; i < ENCRYPTION_KEY_LENGTH; i++)
	{
		if (sscanf(buf + 2 * i, "%2hhx", encryption_key + i) == 0)
		{
			ereport(FATAL,
					(errmsg("Invalid character in encryption key at position %d",
							2 * i)));
		}
	}

	pfree(buf);
}
#endif							/* FRONTEND */

/*
 * Initialize encryption subsystem for use. Must be called before any
 * encryptable data is read from or written to data directory.
 */
void
setup_encryption(void)
{
#ifdef USE_ENCRYPTION
	/*
	 * Setup OpenSSL.
	 *
	 * None of these functions should return a value or raise error.
	 */
	ERR_load_crypto_strings();
	OpenSSL_add_all_algorithms();

	/*
	 * TODO Find out if this needs to be called for OpenSSL < 1.1.0.
	 */
	/* OPENSSL_config(NULL); */

	init_encryption_context(&ctx_encrypt, false);
	init_encryption_context(&ctx_decrypt, false);

	init_encryption_context(&ctx_encrypt_stream, true);
	init_encryption_context(&ctx_decrypt_stream, true);

	encryption_setup_done = true;
#else  /* !USE_ENCRYPTION */
#ifndef FRONTEND
	/*
	 * If no encryption implementation is linked and caller requests
	 * encryption, we should error out here and thus cause the calling process
	 * to fail (preferably postmaster, so the child processes don't make the
	 * same mistake).
	 */
	ereport(FATAL, (errmsg(ENCRYPTION_NOT_SUPPORTED_MSG)));
#else
	/* Front-end shouldn't actually get here, but be careful. */
	fprintf(stderr, "%s\n", ENCRYPTION_NOT_SUPPORTED_MSG);
	exit(EXIT_FAILURE);
#endif	/* FRONTEND */
#endif							/* USE_ENCRYPTION */
}

/*
 * Encrypts a fixed value into *buf to verify that encryption key is correct.
 * Caller provided buf needs to be able to hold at least ENCRYPTION_SAMPLE_SIZE
 * bytes.
 */
void
sample_encryption(char *buf)
{
	char		tweak[TWEAK_SIZE];
	int			i;

	for (i = 0; i < TWEAK_SIZE; i++)
		tweak[i] = i;

	encrypt_block("postgresqlcrypt", buf, ENCRYPTION_SAMPLE_SIZE, tweak,
				  false);
}

/*
 * Encrypts one block of data with a specified tweak value. May only be called
 * when encryption_enabled is true.
 *
 * Input and output buffer may point to the same location.
 *
 * "size" must be a (non-zero) multiple of ENCRYPTION_BLOCK.
 *
 * "tweak" value must be TWEAK_SIZE bytes long.
 *
 * If "stream" is set, stream cipher is used instead of block one.
 *
 * All-zero blocks are not encrypted to correctly handle relation extension,
 * and also to simplify handling of holes created by seek past EOF and
 * consequent write (see buffile.c).
 */
void
encrypt_block(const char *input, char *output, Size size, char *tweak,
			  bool stream)
{
#ifdef USE_ENCRYPTION
	int			out_size;
	EVP_CIPHER_CTX *ctx;

	Assert(data_encrypted);

	/*
	 * Block cipher should only be used if the size is whole multiple of
	 * encryption block size.
	 */
	Assert((size >= ENCRYPTION_BLOCK && size % ENCRYPTION_BLOCK == 0) ||
		   stream);

	/*
	 * Empty page is not worth encryption. Do not waste cycles checking for
	 * stream cipher as this is currently used only for XLOG pages, and empty
	 * XLOG page should not be written to disk.
	 */
	if (!stream && IsAllZero(input, size))
	{
		memset(output, 0, size);
		return;
	}

	ctx = !stream ? ctx_encrypt : ctx_encrypt_stream;

	/* The remaining initialization. */
	if (EVP_EncryptInit_ex(ctx, NULL, NULL, encryption_key,
						   (unsigned char *) tweak) != 1)
		evp_error();

	/* Do the actual encryption. */
	if (EVP_EncryptUpdate(ctx, (unsigned char *) output,
						  &out_size, (unsigned char *) input, size) != 1)
		evp_error();

	Assert(out_size == size);
#else
	/* data_encrypted should not be set */
	Assert(false);
#endif							/* USE_ENCRYPTION */
}


#ifdef USE_ENCRYPTION
/*
 * Initialize the OpenSSL context for passed cipher.
 *
 * On server side this happens during postmaster startup, so other processes
 * inherit the initialized context via fork(). There's no reason to this again
 * and again in encrypt_block() / decrypt_block(), also because we cannot
 * handle out-of-memory conditions encountered by OpenSSL in another way than
 * ereport(FATAL). The OOM is much less likely to happen during postmaster
 * startup, and even if it happens, troubleshooting should be easier than if
 * it happened during normal operation.
 *
 * XXX Do we need to call EVP_CIPHER_CTX_cleanup() (via on_proc_exit callback
 * for server processes and other way for front-ends)? Not sure it's
 * necessary, as the initialization does not involve any shared resources
 * (e.g. files).
 */
static void
init_encryption_context(EVP_CIPHER_CTX **ctx_p, bool stream)
{
	EVP_CIPHER_CTX *ctx;
	const EVP_CIPHER *cipher;
#ifdef USE_ASSERT_CHECKING
	int			block_size;
#endif							/* USE_ASSERT_CHECKING */

	cipher = !stream ? EVP_aes_256_cbc() : EVP_aes_256_ctr();

	if ((*ctx_p = EVP_CIPHER_CTX_new()) == NULL)
		evp_error();
	ctx = *ctx_p;
	if (EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1)
		evp_error();

	/*
	 * No padding is needed. For a block cipher, the input block size should
	 * already be a multiple of ENCRYPTION_BLOCK. For stream cipher, we don't
	 * need padding anyway. This might save some cycles at the OpenSSL end.
	 * XXX Is it setting worth when we don't call EVP_DecryptFinal_ex()
	 * anyway?
	 */
	EVP_CIPHER_CTX_set_padding(ctx, 0);

	Assert(EVP_CIPHER_CTX_iv_length(ctx) == TWEAK_SIZE);
	Assert(EVP_CIPHER_CTX_key_length(ctx) == ENCRYPTION_KEY_LENGTH);
	block_size = EVP_CIPHER_CTX_block_size(ctx);
#ifdef USE_ASSERT_CHECKING
	if (!stream)
		Assert(block_size == ENCRYPTION_BLOCK);
	else
		Assert(block_size == 1);
#endif							/* USE_ASSERT_CHECKING */
}

#endif							/* USE_ENCRYPTION */

#ifdef USE_ENCRYPTION
/*
 * Error callback for openssl.
 */
static void
evp_error(void)
{
	ERR_print_errors_fp(stderr);
#ifndef FRONTEND

	/*
	 * FATAL is the appropriate level because backend can hardly fix anything
	 * if encryption / decryption has failed.
	 *
	 * XXX Do we yet need EVP_CIPHER_CTX_cleanup() here?
	 */
	elog(FATAL, "OpenSSL encountered error during encryption or decryption.");
#else
	fprintf(stderr,
			"OpenSSL encountered error during encryption or decryption.");
	exit(EXIT_FAILURE);
#endif							/* FRONTEND */
}
#endif							/* USE_ENCRYPTION */
