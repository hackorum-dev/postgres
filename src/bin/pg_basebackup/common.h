/*-------------------------------------------------------------------------
 *
 * common.h
 *
 * Common declarations shared across all tools of src/bin/pg_basebackup/.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/common.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BASEBACKUP_COMMON_H
#define BASEBACKUP_COMMON_H

/* Types of compression supported */
typedef enum
{
	COMPRESSION_GZIP,
	COMPRESSION_LZ4,
	COMPRESSION_NONE
} DataCompressionMethod;

#endif							/* BASEBACKUP_COMMON_H */
