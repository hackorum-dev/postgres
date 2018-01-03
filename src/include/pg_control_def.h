#ifndef PG_CONTROL_DEF_H
#define PG_CONTROL_DEF_H

#define KB                      1024
#define MB                      (1024 * 1024)
#define GB                      (1024 * 1024 * 1024)

/*
 * Relation definitions
 *
 * Relation block file size must be a power of 2. Maximum value is 2 ^ 15.
 * This is determined by the 15-bit widths of the lp_off and lp_len fields 
 * in ItemIdData (see include/storage/itemid.h).
 */
#define REL_BLCK_SIZE_MIN       KB				/* in bytes */
#define REL_BLCK_SIZE_DEF       (8 * KB)			/* in bytes */
#define REL_BLCK_SIZE_MAX       (32 * KB)			/* in bytes */

#define REL_FILE_SIZE_MIN       GB				/* in bytes */
#define REL_FILE_SIZE_DEF       (2 * (unsigned long) GB)	/* in bytes */
#define REL_FILE_SIZE_MAX       (64 * (unsigned long) GB)	/* in bytes */

/* Below are based on above 2 series of block size and segment size */
#define REL_FILE_BLCK_MIN	1024				/* in blocks. Makes segment size between 1MB and 32MB */
#define REL_FILE_BLCK_DEF	131072          		/* in blocks. Makes segment size between 128MB and 1GB (blck_size = 8KB) */
#define REL_FILE_BLCK_MAX	2097152        			/* in blocks. 2GB */

/*
 * Wal definitions
 */
#define WAL_BLCK_SIZE_MIN       KB				/* in bytes */
#define WAL_BLCK_SIZE_DEF       (8 * KB)			/* in bytes */
#define WAL_BLCK_SIZE_MAX       (64 * KB)			/* in bytes */

#define WAL_FILE_SIZE_MIN       MB              		/* in bytes */
#define WAL_FILE_SIZE_DEF       (16 * MB)      			/* in bytes */
#define WAL_FILE_SIZE_MAX       (unsigned long) GB		/* in bytes */

/* Below are based on above 2 series of block size and segment size */
#define WAL_FILE_BLCK_MIN	16				/* in blocks. 1MB / 64 KB */
#define WAL_FILE_BLCK_DEF	2048				/* in blocks. 16 MB / 8 KB */
#define WAL_FILE_BLCK_MAX	1048576				/* in blocks. 1GB / 1KB */

#endif
