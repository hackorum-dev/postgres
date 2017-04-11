/*-------------------------------------------------------------------------
 *
 * walmethods.h
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/walmethods.h
 *-------------------------------------------------------------------------
 */


typedef void *Walfile;

typedef enum
{
	CLOSE_NORMAL,
	CLOSE_UNLINK,
	CLOSE_NO_RENAME
}	WalCloseMethod;

/*
 * A WalWriteMethod structure represents the different methods used
 * to write the streaming WAL as it's received.
 *
 * All methods that have a failure path will set errno on failure.
 */
typedef struct WalWriteMethod WalWriteMethod;
struct WalWriteMethod
{
	/* Open a target file. Returns Walfile, or NULL if open failed. */
	Walfile(*open_for_write) (const char *pathname, const char *temp_suffix, size_t pad_to_size);

	/*
	 * Close an open Walfile, using one or more methods for handling
	 * automatic unlinking etc. Returns 0 on success, other values
	 * for error.
	 */
	int			(*close) (Walfile f, WalCloseMethod method);

	/* Check if a file exist */
	bool		(*existsfile) (const char *pathname);

	/* Return the size of a file, or -1 on failure. */
	ssize_t		(*get_file_size) (const char *pathname);

	/*
	 * Write count number of bytes to the file, and return the number
	 * of bytes actually written or -1 for error.
	 */
	ssize_t		(*write) (Walfile f, const void *buf, size_t count);

	/* Return the current position in a file or -1 on error */
	off_t		(*get_current_pos) (Walfile f);

	/*
	 * fsync the contents of the specified file. Returns 0 on
	 * success.
	 */
	int			(*sync) (Walfile f);

	/* Clean up the Walmethod, closing any shared resources */
	bool		(*finish) (void);

	/* Return a text for the last error in this Walfile */
	char	   *(*getlasterror) (void);
};

/*
 * Available WAL methods:
 *	- WalDirectoryMethod - write WAL to regular files in a standard pg_xlog
 *	- TarDirectoryMethod - write WAL to a tarfile corresponding to pg_xlog
 *						   (only implements the methods required for pg_basebackup,
 *						   not all those required for pg_receivewal)
 */
WalWriteMethod *CreateWalDirectoryMethod(const char *basedir,
										 int compression, bool sync);
WalWriteMethod *CreateWalTarMethod(const char *tarbase, int compression, bool sync);

/* Cleanup routines for previously-created methods */
void FreeWalDirectoryMethod(void);
void FreeWalTarMethod(void);
