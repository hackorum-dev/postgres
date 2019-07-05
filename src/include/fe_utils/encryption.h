/*-------------------------------------------------------------------------
 *
 * encryption.h
 *	  Client code to support full cluster encryption.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/fe_utils/encryption.h
 *
 *-------------------------------------------------------------------------
 */

/* Executable to retrieve the encryption key. */
extern char *encryption_key_command;

extern void run_encryption_key_command(unsigned char *encryption_key);
