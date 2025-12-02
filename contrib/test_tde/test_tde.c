/*-------------------------------------------------------------------------
 *
 * test_tde.c
 *		Reference implementation for Storage I/O Transform Hooks
 *
 * WARNING: This is for TESTING ONLY. Do not use in production.
 *	- Key stored in plaintext GUC
 *	- No key rotation
 *	- Minimal error handling
 *	- Not audited for security
 *
 * For production TDE, use a dedicated extension project.
 *
 * This extension demonstrates how to use the storage I/O transform hooks
 * for transparent data encryption. It uses AES-256-CTR for encryption
 * with IV derived from page metadata and block location.
 *
 * Author: Henson Choi <assam258@gmail.com>
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/test_tde/test_tde.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <string.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "catalog/pg_tablespace_d.h"
#include "fmgr.h"
#include "port/pg_crc32c.h"
#include "access/xlog.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"
#include "storage/md.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_tde",
					.version = PG_VERSION
);

/* ----------
 * GUC variables
 * ----------
 */
static char *test_tde_key_hex = NULL;	/* 64 hex chars = 256 bits */

/* ----------
 * Module state
 * ----------
 */

/*
 * Memory context for encryption buffers.
 * Allows allocation in critical sections (for WAL encryption).
 */
static MemoryContext test_tde_cxt = NULL;

/*
 * Transform ID for this extension.
 * Value 1 means page is encrypted with test_tde.
 * Value 0 means page is not transformed (plaintext).
 */
#define TEST_TDE_TRANSFORM_ID	1

/*
 * Dynamic buffers for encrypted pages.
 * Grows as needed, freed in _PG_fini.
 */
static char *encrypt_buffer = NULL;
static const void **encrypt_buffer_ptrs = NULL;
static BlockNumber encrypt_buffer_nblocks = 0;

/*
 * WAL encryption buffer - allocated from test_tde_cxt which allows
 * allocation in critical sections via MemoryContextAllowInCriticalSection().
 */
static char *wal_encrypt_buffer = NULL;
static Size wal_encrypt_buffer_size = 0;

/*
 * WAL decryption buffer - static, only needed for records within a single page.
 * When inplace_allowed=false, record doesn't cross page boundary, so max size
 * is XLOG_BLCKSZ.
 */
static char wal_decrypt_buffer[XLOG_BLCKSZ];

/*
 * Pre-allocated OpenSSL cipher context.
 * Created in _PG_init() and reused for all encrypt/decrypt operations.
 * This avoids memory allocation in critical sections.
 */
static EVP_CIPHER_CTX *cipher_ctx = NULL;

/*
 * Transformed WAL record structure (using XLR_BLOCK_ID_TRANSFORMED from xlogrecord.h):
 *   [XLogRecord header]
 *   [block_id=251 (1B)]
 *   [payload_length (4B)]
 *   [IV (16B)]
 *   [encrypted payload]
 *
 * The block ID 251 marks this record as transformed. After decryption,
 * the marker, length, and IV are removed, restoring the original structure.
 * If decryption is not performed, the unknown block ID causes parse failure.
 *
 * Note: The 21-byte overhead may temporarily cause xl_tot_len to exceed
 * XLogRecordMaxSize after encryption. This is safe because:
 * - XLogRecordMaxSize is only checked in XLogRecordAssemble() before our hook
 * - XLogInsertRecord() does not re-validate the size
 * - The decode hook removes the overhead before WAL parsing, restoring the
 *   original size which was already validated
 */
#define WAL_ENCRYPT_IV_SIZE			16
#define WAL_ENCRYPT_OVERHEAD		(SizeOfXLogRecordDataHeaderLong + WAL_ENCRYPT_IV_SIZE)
#define WAL_CRC_SIZE			sizeof(pg_crc32c)	/* 4 bytes */
#define WAL_IV_RANDOM_SIZE		(WAL_ENCRYPT_IV_SIZE - WAL_CRC_SIZE)	/* 12 bytes */

/* Static XLogRecData for returning encrypted WAL */
static XLogRecData wal_rdata_head;

/* Previous hook values (for chaining) */
static mdread_post_hook_type prev_mdread_post_hook = NULL;
static mdwrite_pre_hook_type prev_mdwrite_pre_hook = NULL;
static mdextend_pre_hook_type prev_mdextend_pre_hook = NULL;
static xlog_insert_pre_hook_type prev_xlog_insert_pre_hook = NULL;
static xlog_decode_pre_hook_type prev_xlog_decode_pre_hook = NULL;

/* ----------
 * Function declarations
 * ----------
 */

/* Module entry points */
void		_PG_init(void);
void		_PG_fini(void);

/* GUC callbacks */
static bool check_test_tde_key(char **newval, void **extra, GucSource source);

/* Hook functions */
static void test_tde_mdread_post(RelFileLocator *rlocator, ForkNumber forknum,
								 BlockNumber blocknum, void **buffers,
								 BlockNumber nblocks);
static const void **test_tde_mdwrite_pre(RelFileLocator *rlocator,
										 ForkNumber forknum,
										 BlockNumber blocknum,
										 const void **buffers,
										 BlockNumber nblocks);
static const void *test_tde_mdextend_pre(RelFileLocator *rlocator,
										 ForkNumber forknum,
										 BlockNumber blocknum,
										 const void *buffer);
static struct XLogRecData *test_tde_xlog_insert_pre(struct XLogRecData *rdata);
static XLogRecord *test_tde_xlog_decode_pre(XLogReaderState *state,
											XLogRecord *record,
											XLogRecPtr lsn,
											bool inplace_allowed);

/* Internal helper functions */
static void ensure_encrypt_buffer(BlockNumber nblocks);
static bool parse_hex_key(const char *hex, unsigned char *out, int outlen);
static void derive_iv(unsigned char *iv, RelFileLocator *rlocator,
					  BlockNumber blocknum, XLogRecPtr lsn);
static void transform_data(const unsigned char *in, unsigned char *out,
						   int len, const unsigned char *iv);
static bool should_transform(RelFileLocator *rlocator, ForkNumber forknum);


/* ----------
 * Internal helper functions
 * ----------
 */

/*
 * Parse hex string to bytes
 */
static bool
parse_hex_key(const char *hex, unsigned char *out, int outlen)
{
	int			i;
	int			hexlen;

	if (hex == NULL)
		return false;

	hexlen = strlen(hex);
	if (hexlen != outlen * 2)
		return false;

	for (i = 0; i < outlen; i++)
	{
		int			hi,
					lo;
		char		c;

		c = hex[i * 2];
		if (c >= '0' && c <= '9')
			hi = c - '0';
		else if (c >= 'a' && c <= 'f')
			hi = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			hi = c - 'A' + 10;
		else
			return false;

		c = hex[i * 2 + 1];
		if (c >= '0' && c <= '9')
			lo = c - '0';
		else if (c >= 'a' && c <= 'f')
			lo = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			lo = c - 'A' + 10;
		else
			return false;

		out[i] = (hi << 4) | lo;
	}

	return true;
}

/*
 * Ensure encrypt buffer can hold 'nblocks' pages.
 * Grows by 2x when needed. Uses test_tde_cxt for persistence.
 */
static void
ensure_encrypt_buffer(BlockNumber nblocks)
{
	if (encrypt_buffer_nblocks >= nblocks)
		return;

	if (encrypt_buffer == NULL)
	{
		BlockNumber initial = Max(8, nblocks);
		Size		size = (Size) initial * BLCKSZ;

		encrypt_buffer = MemoryContextAllocAligned(test_tde_cxt, size,
												   PG_IO_ALIGN_SIZE, 0);
		encrypt_buffer_ptrs = MemoryContextAlloc(test_tde_cxt,
												 initial * sizeof(void *));
		encrypt_buffer_nblocks = initial;
	}
	else
	{
		BlockNumber new_nblocks = encrypt_buffer_nblocks;
		Size		new_size;

		while (new_nblocks < nblocks)
			new_nblocks *= 2;

		new_size = (Size) new_nblocks * BLCKSZ;

		/* repalloc doesn't preserve alignment, so allocate new and copy */
		{
			char	   *new_buffer = MemoryContextAllocAligned(test_tde_cxt,
															   new_size,
															   PG_IO_ALIGN_SIZE, 0);

			memcpy(new_buffer, encrypt_buffer,
				   (Size) encrypt_buffer_nblocks * BLCKSZ);
			pfree(encrypt_buffer);
			encrypt_buffer = new_buffer;
		}

		encrypt_buffer_ptrs = repalloc(encrypt_buffer_ptrs,
									   new_nblocks * sizeof(void *));
		encrypt_buffer_nblocks = new_nblocks;
	}

	/* Update pointers array */
	for (BlockNumber i = 0; i < encrypt_buffer_nblocks; i++)
		encrypt_buffer_ptrs[i] = encrypt_buffer + (Size) i * BLCKSZ;
}


/*
 * Derive IV from page location and header
 *
 * IV structure (16 bytes) - simple, deterministic layout:
 *
 * AES-CTR mode only requires IV uniqueness, not randomness.
 * The combination of LSN + RelFileNumber + BlockNumber guarantees uniqueness:
 *   - LSN: Globally unique across entire WAL stream
 *   - RelFileNumber: Unique within database
 *   - BlockNumber: Unique within relation
 *
 * Even when a single WAL record modifies multiple pages (e.g., B-tree split),
 * the BlockNumber distinguishes each page.
 *
 * Layout (high entropy bytes first, low entropy bytes last for CTR counter space):
 *   [0-3]   LSN low 32 bits - changes frequently (high entropy)
 *   [4-5]   LSN bits 32-47 - mid entropy
 *   [6-8]   BlockNumber low 24 bits
 *   [9-11]  RelFileNumber low 24 bits
 *   [12]    BlockNumber high 8 bits - usually 0 for small tables
 *   [13]    RelFileNumber high 8 bits - usually 0
 *   [14-15] LSN bits 48-63 - usually 0, counter space for CTR
 *
 * CTR counter space analysis:
 *   - Page size: 8KB, encrypted area: 8168 bytes (excluding 24-byte header)
 *   - AES block size: 16 bytes
 *   - Counter increments per page: 8168/16 = 511 (0x1FF)
 *   - Counter affects only IV[14-15] (max increment 0x1FF < 0x10000)
 *   - Bytes 12-15 provide 2^32 counter space, far exceeding 511 needed
 *   - Collision requires same IV[0-11], which means same LSN+BlockNum+RelNum
 *
 * Note: spcOid, dbOid not used - RelFileNumber is sufficient for uniqueness.
 *
 * Known limitation: Operations that copy/move files while changing
 * RelFileNumber without going through storage hooks cause decryption failure.
 */
static void
derive_iv(unsigned char *iv, RelFileLocator *rlocator,
		  BlockNumber blocknum, XLogRecPtr lsn)
{

	/*
	 * Layout: High entropy first, low entropy (usually 0) last.
	 * [LSN low 4B][LSN mid 2B][BlockNum low 3B][RelNum low 3B]
	 * [BlockNum high 1B][RelNum high 1B][LSN high 2B]
	 */

	/* LSN low 32 bits - bytes 0-3 (high entropy, changes frequently) */
	iv[0] = (uint8) ((lsn >> 0) & 0xFF);
	iv[1] = (uint8) ((lsn >> 8) & 0xFF);
	iv[2] = (uint8) ((lsn >> 16) & 0xFF);
	iv[3] = (uint8) ((lsn >> 24) & 0xFF);

	/* LSN bits 32-47 - bytes 4-5 (mid entropy) */
	iv[4] = (uint8) ((lsn >> 32) & 0xFF);
	iv[5] = (uint8) ((lsn >> 40) & 0xFF);

	/* BlockNumber low 24 bits - bytes 6-8 */
	iv[6] = (uint8) ((blocknum >> 0) & 0xFF);
	iv[7] = (uint8) ((blocknum >> 8) & 0xFF);
	iv[8] = (uint8) ((blocknum >> 16) & 0xFF);

	/* RelFileNumber low 24 bits - bytes 9-11 */
	iv[9] = (uint8) ((rlocator->relNumber >> 0) & 0xFF);
	iv[10] = (uint8) ((rlocator->relNumber >> 8) & 0xFF);
	iv[11] = (uint8) ((rlocator->relNumber >> 16) & 0xFF);

	/* BlockNumber high 8 bits - byte 12 (usually 0 for small tables) */
	iv[12] = (uint8) ((blocknum >> 24) & 0xFF);

	/* RelFileNumber high 8 bits - byte 13 (usually 0) */
	iv[13] = (uint8) ((rlocator->relNumber >> 24) & 0xFF);

	/* LSN bits 48-63 - bytes 14-15 (usually 0, counter space for CTR) */
	iv[14] = (uint8) ((lsn >> 48) & 0xFF);
	iv[15] = (uint8) ((lsn >> 56) & 0xFF);
}

/*
 * Encrypt or decrypt data using AES-256-CTR
 *
 * AES-CTR is symmetric: encrypt and decrypt use the same operation.
 */
static void
transform_data(const unsigned char *in, unsigned char *out, int len,
			   const unsigned char *iv)
{
	int			outlen,
				tmplen;

	if (len <= 0)
		return;

	/*
	 * cipher_ctx is pre-allocated and initialized with cipher/key in _PG_init().
	 * Here we only set IV (cipher=NULL, key=NULL), which avoids internal
	 * memory allocation. This is critical for WAL encryption which runs
	 * inside critical sections. We use PANIC for all errors.
	 */
	if (cipher_ctx == NULL)
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("test_tde: cipher context not initialized")));

	if (EVP_EncryptInit_ex(cipher_ctx, NULL, NULL, NULL, iv) != 1)
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("test_tde: EVP_EncryptInit_ex failed: %s",
						ERR_error_string(ERR_get_error(), NULL))));

	if (EVP_EncryptUpdate(cipher_ctx, out, &outlen, in, len) != 1)
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("test_tde: EVP_EncryptUpdate failed: %s",
						ERR_error_string(ERR_get_error(), NULL))));

	if (EVP_EncryptFinal_ex(cipher_ctx, out + outlen, &tmplen) != 1)
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("test_tde: EVP_EncryptFinal_ex failed: %s",
						ERR_error_string(ERR_get_error(), NULL))));
}

/*
 * Check if we should encrypt/decrypt this relation
 *
 * For this test implementation, we encrypt only user-created relations.
 * A production implementation would check encryption policies.
 */
static bool
should_transform(RelFileLocator *rlocator, ForkNumber forknum)
{
	/* Skip if cipher not initialized (key not configured) */
	if (cipher_ctx == NULL)
		return false;

	/* Skip system catalog tablespace (pg_global) */
	if (rlocator->spcOid == GLOBALTABLESPACE_OID)
		return false;

	/*
	 * Skip system catalogs (OID < FirstNormalObjectId). This ensures we don't
	 * try to encrypt/decrypt pre-existing system catalog pages that were
	 * created without encryption.
	 */
	if (rlocator->relNumber < FirstNormalObjectId)
		return false;

	(void) forknum;				/* all forks are encrypted for user tables */

	return true;
}


/* ----------
 * Hook functions - Page I/O
 * ----------
 */

/*
 * Post-read hook: decrypt blocks after reading from disk
 */
static void
test_tde_mdread_post(RelFileLocator *rlocator, ForkNumber forknum,
					 BlockNumber blocknum, void **buffers,
					 BlockNumber nblocks)
{
	BlockNumber i;
	unsigned char iv[16];

	/* Chain to previous hook if any */
	if (prev_mdread_post_hook)
		prev_mdread_post_hook(rlocator, forknum, blocknum, buffers, nblocks);

	for (i = 0; i < nblocks; i++)
	{
		PageHeader	phdr = (PageHeader) buffers[i];
		uint16		checksum;
		uint8		transform_id;

		/* Skip empty/new pages */
		if (PageIsNew((Page) buffers[i]))
			continue;

		/* Skip if page doesn't look valid */
		if (phdr->pd_lower < SizeOfPageHeaderData ||
			phdr->pd_lower > phdr->pd_upper ||
			phdr->pd_upper > phdr->pd_special ||
			phdr->pd_special > BLCKSZ)
			continue;

		/* Check transform ID - skip if page is not encrypted by us */
		transform_id = PageGetTransformId((Page) buffers[i]);
		if (transform_id == PD_TRANSFORM_NONE)
			continue;	/* Page is not encrypted */

		if (transform_id != TEST_TDE_TRANSFORM_ID)
		{
			elog(DEBUG1, "test_tde: skipping block %u with transform ID %u (not ours)",
				 blocknum + i, transform_id);
			continue;
		}

		/* Page is encrypted but cipher not initialized - fatal error */
		if (cipher_ctx == NULL)
			ereport(PANIC,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("test_tde: encrypted page found but encryption key not configured"),
					 errdetail("Block %u of relation %u/%u/%u fork %d has transform ID %u.",
							   blocknum + i, rlocator->spcOid, rlocator->dbOid,
							   rlocator->relNumber, forknum, transform_id)));

		/* Verify checksum on encrypted data before decryption */
		if (DataChecksumsEnabled())
		{
			checksum = pg_checksum_page((char *) buffers[i], blocknum + i);
			if (checksum != phdr->pd_checksum)
			{
				ereport(WARNING,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("page verification failed, calculated checksum %u but expected %u",
								checksum, phdr->pd_checksum)));
			}
		}

		/* Derive IV using LSN from page header */
		derive_iv(iv, rlocator, blocknum + i, PageGetLSN((Page) buffers[i]));

		/* Decrypt data area in place (header stays unchanged) */
		transform_data((unsigned char *) buffers[i] + SizeOfPageHeaderData,
					   (unsigned char *) buffers[i] + SizeOfPageHeaderData,
					   BLCKSZ - SizeOfPageHeaderData, iv);

		/* Clear transform ID and recalculate checksum for plaintext data */
		PageSetTransformId((Page) buffers[i], PD_TRANSFORM_NONE);
		PageSetChecksumInplace((Page) buffers[i], blocknum + i);
	}
}

/*
 * Helper: encrypt a single page into the encrypt_buffer at given offset.
 * Returns pointer to encrypted page, or original buffer if page was skipped.
 */
static const void *
encrypt_page(RelFileLocator *rlocator, BlockNumber blocknum,
			 const void *buffer, Size buffer_offset)
{
	unsigned char iv[16];
	PageHeader	phdr = (PageHeader) buffer;
	char	   *dest = encrypt_buffer + buffer_offset;

	/* Skip empty/new pages */
	if (PageIsNew((Page) buffer))
		return buffer;

	/* Skip if page doesn't look valid */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		return buffer;

	/* Derive IV using LSN from page header */
	derive_iv(iv, rlocator, blocknum, PageGetLSN((Page) buffer));

	/* Copy header, encrypt data area */
	memcpy(dest, buffer, SizeOfPageHeaderData);
	transform_data((unsigned char *) buffer + SizeOfPageHeaderData,
				   (unsigned char *) dest + SizeOfPageHeaderData,
				   BLCKSZ - SizeOfPageHeaderData, iv);

	/* Set transform ID to mark page as encrypted */
	PageSetTransformId((Page) dest, TEST_TDE_TRANSFORM_ID);

	/* Recalculate checksum for encrypted data */
	PageSetChecksumInplace((Page) dest, blocknum);

	return dest;
}

/*
 * Pre-write hook: encrypt blocks before writing to disk
 */
static const void **
test_tde_mdwrite_pre(RelFileLocator *rlocator, ForkNumber forknum,
					 BlockNumber blocknum, const void **buffers,
					 BlockNumber nblocks)
{
	BlockNumber i;

	/* Chain to previous hook if any */
	if (prev_mdwrite_pre_hook)
		buffers = prev_mdwrite_pre_hook(rlocator, forknum, blocknum, buffers, nblocks);

	if (!should_transform(rlocator, forknum))
		return buffers;

	/* Ensure buffer is large enough */
	ensure_encrypt_buffer(nblocks);

	for (i = 0; i < nblocks; i++)
		encrypt_buffer_ptrs[i] = encrypt_page(rlocator, blocknum + i,
											  buffers[i], (Size) i * BLCKSZ);

	return encrypt_buffer_ptrs;
}

/*
 * Pre-extend hook: encrypt block before extending relation
 */
static const void *
test_tde_mdextend_pre(RelFileLocator *rlocator, ForkNumber forknum,
					  BlockNumber blocknum, const void *buffer)
{
	/* Chain to previous hook if any */
	if (prev_mdextend_pre_hook)
		buffer = prev_mdextend_pre_hook(rlocator, forknum, blocknum, buffer);

	if (!should_transform(rlocator, forknum))
		return buffer;

	/* Ensure buffer is large enough for at least 1 block */
	ensure_encrypt_buffer(1);

	return encrypt_page(rlocator, blocknum, buffer, 0);
}


/* ----------
 * Hook functions - WAL I/O
 * ----------
 */

/*
 * Ensure WAL encryption buffer is large enough.
 * Uses test_tde_cxt which allows allocation in critical sections.
 */
static void
ensure_wal_encrypt_buffer(Size needed)
{
	if (wal_encrypt_buffer_size >= needed)
		return;

	if (wal_encrypt_buffer == NULL)
		wal_encrypt_buffer = MemoryContextAlloc(test_tde_cxt, needed);
	else
		wal_encrypt_buffer = repalloc(wal_encrypt_buffer, needed);
	wal_encrypt_buffer_size = needed;
}

/*
 * WAL insert pre-hook: encrypt WAL record data
 *
 * Strategy:
 * 1. Copy XLogRecord header and payload
 * 2. Save plaintext CRC from header (xl_crc contains payload CRC at this point)
 * 3. Build IV: [plaintext CRC (4B)] [random (12B)]
 * 4. Insert transformation header (block ID 251 + payload_length) and IV
 * 5. Encrypt original payload with the IV
 * 6. Update xl_tot_len and recalculate CRC for encrypted payload
 *
 * Resulting record structure:
 *   [XLogRecord header]
 *   [block_id=251 (1B)]
 *   [payload_length (4B)]
 *   [IV 16B]
 *   [encrypted payload]
 *
 * The block ID 251 marks this record as encrypted. After decryption,
 * the marker, length, and IV are removed, restoring the original structure.
 * If decryption is not performed, the unknown block ID causes parse failure.
 */
static struct XLogRecData *
test_tde_xlog_insert_pre(struct XLogRecData *rdata)
{
	XLogRecData *node;
	XLogRecord *rechdr;
	char	   *bufptr;
	char	   *new_payload_start;
	uint32		orig_total_len;
	uint32		orig_payload_len;
	uint32		new_total_len;
	uint32		transform_payload_len;
	unsigned char iv[WAL_ENCRYPT_IV_SIZE];
	pg_crc32c	plaintext_crc;

	/* Chain to previous hook if any */
	if (prev_xlog_insert_pre_hook)
		rdata = prev_xlog_insert_pre_hook(rdata);

	/* Skip if cipher not initialized (key not configured) */
	if (cipher_ctx == NULL)
		return rdata;

	/* First node must contain XLogRecord header */
	if (rdata == NULL || rdata->data == NULL || rdata->len < SizeOfXLogRecord)
		return rdata;

	rechdr = (XLogRecord *) rdata->data;
	orig_total_len = rechdr->xl_tot_len;
	orig_payload_len = orig_total_len - SizeOfXLogRecord;

	/* Sanity check */
	if (orig_total_len < SizeOfXLogRecord)
		return rdata;

	/*
	 * Skip records with no payload (e.g., XLOG_SWITCH). These are header-only
	 * records where adding encryption overhead would break size assertions.
	 */
	if (orig_payload_len == 0)
		return rdata;

	new_total_len = orig_total_len + WAL_ENCRYPT_OVERHEAD;

	/*
	 * Save plaintext CRC before we modify anything.
	 * At this point, xl_crc contains the CRC of the payload only
	 * (header CRC is added later by XLogInsertRecord).
	 */
	plaintext_crc = rechdr->xl_crc;

	/*
	 * Ensure buffer is large enough. test_tde_cxt allows allocation in
	 * critical sections, so this is safe even during WAL insertion.
	 * OOM here will cause PANIC, which is acceptable for critical sections.
	 */
	ensure_wal_encrypt_buffer(new_total_len);

	/*
	 * Build IV: [plaintext CRC (4B)] [random (12B)]
	 * Store CRC directly in IV[0..3] (little-endian).
	 */
	iv[0] = ((uint32) plaintext_crc >> 0) & 0xFF;
	iv[1] = ((uint32) plaintext_crc >> 8) & 0xFF;
	iv[2] = ((uint32) plaintext_crc >> 16) & 0xFF;
	iv[3] = ((uint32) plaintext_crc >> 24) & 0xFF;

	/* Generate random bytes for IV[4..15] (12 bytes) for uniqueness */
	if (!pg_strong_random(iv + WAL_CRC_SIZE, WAL_IV_RANDOM_SIZE))
	{
		ereport(WARNING,
				(errmsg("test_tde: failed to generate random IV for WAL")));
		return rdata;
	}

	/*
	 * Build encrypted record in buffer:
	 * [header][block_id][payload_length][IV][encrypted_payload]
	 */
	bufptr = wal_encrypt_buffer;

	/* 1. Copy header from first rdata node */
	memcpy(bufptr, rdata->data, SizeOfXLogRecord);
	bufptr += SizeOfXLogRecord;

	/* 2. Insert transformation header (block ID 251 + payload_length) */
	new_payload_start = bufptr;
	*bufptr = (char) XLR_BLOCK_ID_TRANSFORMED;
	bufptr += sizeof(uint8);

	/* Calculate payload_length: IV + encrypted payload */
	transform_payload_len = WAL_ENCRYPT_IV_SIZE + orig_payload_len;

	/* Store payload_length (4 bytes, unaligned, little-endian) */
	bufptr[0] = (char) ((transform_payload_len >> 0) & 0xFF);
	bufptr[1] = (char) ((transform_payload_len >> 8) & 0xFF);
	bufptr[2] = (char) ((transform_payload_len >> 16) & 0xFF);
	bufptr[3] = (char) ((transform_payload_len >> 24) & 0xFF);
	bufptr += sizeof(uint32);

	/* 3. Insert IV (CRC in first 4 bytes, random in remaining 12) */
	memcpy(bufptr, iv, WAL_ENCRYPT_IV_SIZE);
	bufptr += WAL_ENCRYPT_IV_SIZE;

	/* 4. Copy payload to buffer, then encrypt in-place */
	if (orig_payload_len > 0)
	{
		Size		first_node_payload;
		char	   *encrypt_start = bufptr;

		/* First node: skip header, copy remaining payload */
		first_node_payload = rdata->len - SizeOfXLogRecord;
		if (first_node_payload > 0)
		{
			memcpy(bufptr, (char *) rdata->data + SizeOfXLogRecord, first_node_payload);
			bufptr += first_node_payload;
		}

		/* Remaining nodes: copy all data */
		for (node = rdata->next; node != NULL; node = node->next)
		{
			if (node->len > 0 && node->data != NULL)
			{
				memcpy(bufptr, node->data, node->len);
				bufptr += node->len;
			}
		}

		/* Encrypt payload in-place */
		transform_data((unsigned char *) encrypt_start,
					   (unsigned char *) encrypt_start,
					   orig_payload_len, iv);
	}

	/* Update header with new total length */
	rechdr = (XLogRecord *) wal_encrypt_buffer;
	rechdr->xl_tot_len = new_total_len;

	/*
	 * Recalculate CRC for the new payload (marker + length + IV + encrypted data).
	 * The header CRC will be added by XLogInsertRecord later.
	 */
	{
		pg_crc32c	crc;

		INIT_CRC32C(crc);
		COMP_CRC32C(crc, new_payload_start, new_total_len - SizeOfXLogRecord);
		rechdr->xl_crc = crc;
	}

	/* Return single XLogRecData pointing to our encrypted buffer */
	wal_rdata_head.next = NULL;
	wal_rdata_head.data = wal_encrypt_buffer;
	wal_rdata_head.len = new_total_len;

	return &wal_rdata_head;
}

/*
 * WAL decode pre-hook: decrypt WAL record data
 *
 * This reverses the encryption done in xlog_insert_pre_hook.
 * Checks for block ID 251 marker to identify encrypted records.
 *
 * Input:  [header] [block_id=251 (1B)] [payload_length (4B)] [IV 16B] [encrypted payload]
 * Output: [header] [original payload] (shorter by 21 bytes)
 *
 * Recovery process:
 * 1. Check for encryption marker (block ID 251)
 * 2. Read payload_length from transform header
 * 3. Extract IV for decryption
 * 4. Decrypt payload using IV
 * 5. Extract plaintext payload CRC from IV[0..3]
 * 6. Restore original record structure
 *
 * If the marker is not found, record is not encrypted (pass through).
 * If inplace_allowed, decrypts in place. Otherwise, copies to static buffer.
 */
static XLogRecord *
test_tde_xlog_decode_pre(XLogReaderState *state, XLogRecord *record,
						 XLogRecPtr lsn, bool inplace_allowed)
{
	uint32		total_len;
	uint32		transform_payload_len;
	uint32		encrypted_payload_len;
	unsigned char iv[WAL_ENCRYPT_IV_SIZE];
	char	   *payload_start;
	char	   *len_ptr;
	XLogRecord *work_record;

	/* Chain to previous hook if any */
	if (prev_xlog_decode_pre_hook)
		record = prev_xlog_decode_pre_hook(state, record, lsn, inplace_allowed);

	if (record == NULL)
		return record;

	total_len = record->xl_tot_len;

	/* Must have at least header + transform header + IV */
	if (total_len < SizeOfXLogRecord + WAL_ENCRYPT_OVERHEAD)
		return record;

	/* Check for transformation marker (block ID 251) */
	payload_start = (char *) record + SizeOfXLogRecord;
	if ((unsigned char) *payload_start != XLR_BLOCK_ID_TRANSFORMED)
		return record;			/* Not transformed, pass through */

	/* WAL is encrypted but cipher not initialized - fatal error */
	if (cipher_ctx == NULL)
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("test_tde: encrypted WAL record found but encryption key not configured"),
				 errdetail("WAL record at LSN %X/%X has transformation marker.",
						   LSN_FORMAT_ARGS(lsn))));

	/*
	 * If inplace modification allowed, work directly on record. Otherwise,
	 * copy to static buffer (record fits in single page).
	 */
	if (inplace_allowed)
	{
		work_record = record;
	}
	else
	{
		/* Record within single page, must fit in XLOG_BLCKSZ */
		if (total_len > XLOG_BLCKSZ)
		{
			ereport(WARNING,
					(errmsg("test_tde: WAL record too large for decryption buffer")));
			return record;
		}
		memcpy(wal_decrypt_buffer, record, total_len);
		work_record = (XLogRecord *) wal_decrypt_buffer;
	}

	/* Recalculate payload_start for work_record */
	payload_start = (char *) work_record + SizeOfXLogRecord;

	/* Read payload_length from transform header (4 bytes, unaligned, little-endian) */
	len_ptr = payload_start + sizeof(uint8);
	transform_payload_len = ((uint32) (unsigned char) len_ptr[0] << 0) |
							((uint32) (unsigned char) len_ptr[1] << 8) |
							((uint32) (unsigned char) len_ptr[2] << 16) |
							((uint32) (unsigned char) len_ptr[3] << 24);

	/* Validate payload_length */
	if (transform_payload_len < WAL_ENCRYPT_IV_SIZE ||
		transform_payload_len > total_len - SizeOfXLogRecord - SizeOfXLogRecordDataHeaderLong)
	{
		ereport(WARNING,
				(errmsg("test_tde: invalid transform payload length %u at LSN %X/%X",
						transform_payload_len, LSN_FORMAT_ARGS(lsn))));
		return record;
	}

	/* Extract IV (after transform header) */
	memcpy(iv, payload_start + SizeOfXLogRecordDataHeaderLong, WAL_ENCRYPT_IV_SIZE);

	/* Encrypted payload length = transform_payload_len - IV */
	encrypted_payload_len = transform_payload_len - WAL_ENCRYPT_IV_SIZE;

	/*
	 * Decrypt payload directly to payload_start position, removing header and IV.
	 * Source: payload_start + 21 (encrypted data after transform header + IV)
	 * Dest:   payload_start (overwrite transform header with decrypted data)
	 */
	if (encrypted_payload_len > 0)
	{
		transform_data((unsigned char *) (payload_start + WAL_ENCRYPT_OVERHEAD),
					   (unsigned char *) payload_start,
					   encrypted_payload_len, iv);
	}

	/* Update header with original length (transform header and IV removed) */
	work_record->xl_tot_len = SizeOfXLogRecord + encrypted_payload_len;

	/*
	 * Recover plaintext payload CRC from IV[0..3] (little-endian).
	 */
	{
		pg_crc32c	recovered_payload_crc;
		pg_crc32c	full_crc;

		/* Extract CRC directly from IV[0..3] */
		recovered_payload_crc = (pg_crc32c) (((uint32) iv[0] << 0) |
											 ((uint32) iv[1] << 8) |
											 ((uint32) iv[2] << 16) |
											 ((uint32) iv[3] << 24));

		/*
		 * For ValidXLogRecord(), we need CRC of: payload + header (up to xl_crc)
		 * The recovered CRC is payload-only, so add header portion.
		 */
		full_crc = recovered_payload_crc;
		COMP_CRC32C(full_crc, (char *) work_record, offsetof(XLogRecord, xl_crc));
		FIN_CRC32C(full_crc);
		work_record->xl_crc = full_crc;
	}

	return work_record;
}


/* ----------
 * GUC callbacks
 * ----------
 */

/*
 * GUC check hook for key
 */
static bool
check_test_tde_key(char **newval, void **extra, GucSource source)
{
	if (*newval == NULL || strlen(*newval) == 0)
		return true;

	if (strlen(*newval) != 64)
	{
		GUC_check_errdetail("Key must be exactly 64 hex characters (256 bits).");
		return false;
	}

	/* Validate hex characters */
	for (int i = 0; i < 64; i++)
	{
		char		c = (*newval)[i];

		if (!((c >= '0' && c <= '9') ||
			  (c >= 'a' && c <= 'f') ||
			  (c >= 'A' && c <= 'F')))
		{
			GUC_check_errdetail("Key must contain only hex characters (0-9, a-f, A-F).");
			return false;
		}
	}

	return true;
}

/* ----------
 * Module entry points
 * ----------
 */

/*
 * Module initialization
 */
void
_PG_init(void)
{
	unsigned char key[32];

	/*
	 * Create memory context for encryption buffers and allow allocation
	 * in critical sections. This is necessary because WAL encryption runs
	 * inside critical sections, and OOM there will cause PANIC anyway.
	 */
	test_tde_cxt = AllocSetContextCreate(TopMemoryContext,
										 "test_tde",
										 ALLOCSET_DEFAULT_SIZES);
	MemoryContextAllowInCriticalSection(test_tde_cxt, true);

	/*
	 * Define GUC for encryption key.
	 *
	 * PGC_POSTMASTER: Key can only be set at server start to prevent
	 * accidental runtime changes.
	 *
	 * WARNING: Once data is encrypted with a key, that same key MUST be used
	 * for the lifetime of the data. Changing the key (even across restarts)
	 * will cause decryption failures and data corruption. This reference
	 * implementation does not support key rotation.
	 */
	DefineCustomStringVariable("test_tde.key",
							   "Encryption key in hex format (64 characters = 256 bits).",
							   "WARNING: Key must never change once data is encrypted!",
							   &test_tde_key_hex,
							   "",
							   PGC_POSTMASTER,
							   GUC_SUPERUSER_ONLY,
							   check_test_tde_key,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("test_tde");

	/*
	 * Parse key and initialize cipher context if key is configured.
	 * cipher_ctx remains NULL if no key is set, disabling encryption.
	 */
	if (test_tde_key_hex != NULL && strlen(test_tde_key_hex) == 64)
	{
		if (!parse_hex_key(test_tde_key_hex, key, 32))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("test_tde: failed to parse encryption key")));

		cipher_ctx = EVP_CIPHER_CTX_new();
		if (!cipher_ctx)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("test_tde: failed to create cipher context")));

		if (EVP_EncryptInit_ex(cipher_ctx, EVP_aes_256_ctr(), NULL, key, NULL) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("test_tde: failed to initialize cipher context")));

		/* Clear key from stack */
		explicit_bzero(key, sizeof(key));
	}

	/* Install hooks (save previous values for chaining) */
	prev_mdread_post_hook = mdread_post_hook;
	mdread_post_hook = test_tde_mdread_post;

	prev_mdwrite_pre_hook = mdwrite_pre_hook;
	mdwrite_pre_hook = test_tde_mdwrite_pre;

	prev_mdextend_pre_hook = mdextend_pre_hook;
	mdextend_pre_hook = test_tde_mdextend_pre;

	prev_xlog_insert_pre_hook = xlog_insert_pre_hook;
	xlog_insert_pre_hook = test_tde_xlog_insert_pre;

	prev_xlog_decode_pre_hook = xlog_decode_pre_hook;
	xlog_decode_pre_hook = test_tde_xlog_decode_pre;

	ereport(LOG,
			(errmsg("test_tde: initialized (WARNING: for testing only!)")));
}

/*
 * Module finalization
 */
void
_PG_fini(void)
{
	/* Restore previous hooks */
	xlog_decode_pre_hook = prev_xlog_decode_pre_hook;
	xlog_insert_pre_hook = prev_xlog_insert_pre_hook;
	mdextend_pre_hook = prev_mdextend_pre_hook;
	mdwrite_pre_hook = prev_mdwrite_pre_hook;
	mdread_post_hook = prev_mdread_post_hook;

	/* Free OpenSSL cipher context (also clears key material) */
	if (cipher_ctx != NULL)
	{
		EVP_CIPHER_CTX_free(cipher_ctx);
		cipher_ctx = NULL;
	}

	/*
	 * Delete memory context - this frees all buffers allocated from it
	 * (encrypt_buffer, encrypt_buffer_ptrs, wal_encrypt_buffer).
	 */
	if (test_tde_cxt != NULL)
	{
		MemoryContextDelete(test_tde_cxt);
		test_tde_cxt = NULL;
	}

	/* Reset buffer pointers */
	encrypt_buffer = NULL;
	encrypt_buffer_ptrs = NULL;
	encrypt_buffer_nblocks = 0;
	wal_encrypt_buffer = NULL;
	wal_encrypt_buffer_size = 0;
}
