/*-------------------------------------------------------------------------
 *
 * stringinfo.c
 *
 * StringInfo provides an extensible string data type (currently limited to a
 * length of 1GB).  It can be used to buffer either ordinary C strings
 * (null-terminated text) or arbitrary binary data.  All storage is allocated
 * with palloc() (falling back to malloc in frontend code).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  src/common/stringinfo.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND

#include "postgres.h"
#include "utils/memutils.h"

#else

#include "postgres_fe.h"

#endif

#include "lib/stringinfo.h"


/*
 * initStringInfoInternal
 *
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 * The initial memory allocation size is specified by 'initsize'.
 * The valid range for 'initsize' is 1 to MaxAllocSize.
 */
static inline void
initStringInfoInternal(StringInfo str, int initsize)
{
	Assert(initsize >= 1 && initsize <= MaxAllocSize);

	str->data = (char *) palloc(initsize);
	str->maxlen = initsize;
	resetStringInfo(str);
}

/*
 * makeStringInfoInternal(int initsize)
 *
 * Create an empty 'StringInfoData' & return a pointer to it.
 * The initial memory allocation size is specified by 'initsize'.
 * The valid range for 'initsize' is 1 to MaxAllocSize.
 */
static inline StringInfo
makeStringInfoInternal(int initsize)
{
	StringInfo	res = palloc_object(StringInfoData);

	initStringInfoInternal(res, initsize);
	return res;
}

/*
 * makeStringInfo
 *
 * Create an empty 'StringInfoData' & return a pointer to it.
 */
StringInfo
makeStringInfo(void)
{
	return makeStringInfoInternal(STRINGINFO_DEFAULT_SIZE);
}

/*
 * makeStringInfoExt(int initsize)
 *
 * Create an empty 'StringInfoData' & return a pointer to it.
 * The initial memory allocation size is specified by 'initsize'.
 * The valid range for 'initsize' is 1 to MaxAllocSize.
 */
StringInfo
makeStringInfoExt(int initsize)
{
	return makeStringInfoInternal(initsize);
}

/*
 * initStringInfo
 *
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 */
void
initStringInfo(StringInfo str)
{
	initStringInfoInternal(str, STRINGINFO_DEFAULT_SIZE);
}

/*
 * initStringInfoExt
 *
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 * The initial memory allocation size is specified by 'initsize'.
 * The valid range for 'initsize' is 1 to MaxAllocSize.
 */
void
initStringInfoExt(StringInfo str, int initsize)
{
	initStringInfoInternal(str, initsize);
}

/*
 * resetStringInfo
 *
 * Reset the StringInfo: the data buffer remains valid, but its
 * previous content, if any, is cleared.
 *
 * Read-only StringInfos as initialized by initReadOnlyStringInfo cannot be
 * reset.
 */
void
resetStringInfo(StringInfo str)
{
	/* don't allow resets of read-only StringInfos */
	Assert(str->maxlen != 0);

	str->data[0] = '\0';
	str->len = 0;
	str->cursor = 0;
}

/*
 * appendStringInfo
 *
 * Format text data under the control of fmt (an sprintf-style format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 */
void
appendStringInfo(StringInfo str, const char *fmt, ...)
{
	int			save_errno = errno;

	for (;;)
	{
		va_list		args;
		int			needed;

		/* Try to format the data. */
		va_start(args, fmt);
		needed = appendStringInfoVA(str, fmt, args);
		va_end(args);

		if (likely(needed == 0))
			break;				/* success */

		/* Increase the buffer size and try again. */
		enlargeStringInfo(str, needed);
		errno = save_errno;
	}
}

/*
 * appendStringInfoVA
 *
 * Attempt to format text data under the control of fmt (an sprintf-style
 * format string) and append it to whatever is already in str.  If successful
 * return zero; if not (because there's not enough space), return an estimate
 * of the space needed, without modifying str.  Typically the caller should
 * pass the return value to enlargeStringInfo() before trying again; see
 * appendStringInfo for standard usage pattern.
 *
 * Caution: callers must be sure to preserve their entry-time errno
 * when looping, in case the fmt contains "%m".
 *
 * XXX This API is ugly, but there seems no alternative given the C spec's
 * restrictions on what can portably be done with va_list arguments: you have
 * to redo va_start before you can rescan the argument list, and we can't do
 * that from here.
 */
inline int
appendStringInfoVA(StringInfo str, const char *fmt, va_list args)
{
	int			avail;
	int			nprinted;

	/*
	 * If there's hardly any space, don't bother trying, just fail to make the
	 * caller enlarge the buffer first.  We have to guess at how much to
	 * enlarge, since we're skipping the formatting work.
	 */
	avail = str->maxlen - str->len;
	if (avail < 16)
		return 32;

	nprinted = vsnprintf(str->data + str->len, (size_t) avail, fmt, args);

	/* We assume failure means the fmt is bogus, hence hard failure is OK */
	if (unlikely(nprinted < 0))
	{
#ifndef FRONTEND
		elog(ERROR, "vsnprintf failed: %m with format string \"%s\"", fmt);
#else
		fprintf(stderr, "vsnprintf failed: %m with format string \"%s\"\n",
				fmt);
		exit(EXIT_FAILURE);
#endif
	}

	if (likely(nprinted < avail))
	{
		/* Success.  Note nprinted does not include trailing null. */
		str->len += nprinted;
		return 0;
	}

	/* Restore the trailing null so that str is unmodified. */
	str->data[str->len] = '\0';

	/*
	 * We assume a C99-compliant vsnprintf, so believe its estimate of the
	 * required space.  (If it's wrong, the logic will still work, but we may
	 * loop multiple times.)
	 *
	 * Unlike pvsnprintf(), we don't check for overrunning MaxAllocSize,
	 * preferring to leave that to enlargeStringInfo().
	 */
	return nprinted;
}

/*
 * appendStringInfoString
 *
 * Append a null-terminated string to str.
 * Like appendStringInfo(str, "%s", s) but faster.
 */
void
appendStringInfoString(StringInfo str, const char *s)
{
	appendBinaryStringInfo(str, s, strlen(s));
}

/*
 * appendStringInfoChar
 *
 * Append a single byte to str.
 * Like appendStringInfo(str, "%c", ch) but much faster.
 */
void
appendStringInfoChar(StringInfo str, char ch)
{
	/* Make more room if needed */
	if (str->len + 1 >= str->maxlen)
		enlargeStringInfo(str, 1);

	/* OK, append the character */
	str->data[str->len] = ch;
	str->len++;
	str->data[str->len] = '\0';
}

/*
 * appendStringInfoSpaces
 *
 * Append the specified number of spaces to a buffer.
 */
void
appendStringInfoSpaces(StringInfo str, int count)
{
	if (count > 0)
	{
		/* Make more room if needed */
		enlargeStringInfo(str, count);

		/* OK, append the spaces */
		memset(&str->data[str->len], ' ', count);
		str->len += count;
		str->data[str->len] = '\0';
	}
}

/*
 * Append the given string to the shell command being built in str, with
 * shell-style quoting as needed to create exactly one argument.
 *
 * Forbid LF or CR characters, which have scant practical use beyond designing
 * security breaches.  The Windows command shell is unusable as a conduit for
 * arguments containing LF or CR characters.
 *
 * appendStringInfoShell() reports an error and does not return if LF or CR
 * appears; in the backend it does ereport(ERROR), while in frontend code it
 * prints a message and exits.  appendStringInfoShellNoError() omits those
 * characters from the result, and returns false if there were any.
 *
 * If you make any changes here, also update appendShellString and
 * appendShellStringNoError.
 */
void
appendStringInfoShell(StringInfo str, const char *s)
{
	if (!appendStringInfoShellNoError(str, s))
	{
#ifndef FRONTEND
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("shell command argument contains a newline or carriage return: \"%s\"",
						s)));
#else
		fprintf(stderr,
				_("shell command argument contains a newline or carriage return: \"%s\"\n"),
				s);
		exit(EXIT_FAILURE);
#endif
	}
}

bool
appendStringInfoShellNoError(StringInfo str, const char *s)
{
#ifdef WIN32
	int			backslash_run_length = 0;
#endif
	bool		ok = true;
	const char *p;

	/*
	 * Don't bother with adding quotes if the string is nonempty and clearly
	 * contains only safe characters.
	 */
	if (*s != '\0' &&
		strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./:") == strlen(s))
	{
		appendStringInfoString(str, s);
		return ok;
	}

#ifndef WIN32
	appendStringInfoChar(str, '\'');
	for (p = s; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			ok = false;
			continue;
		}

		if (*p == '\'')
			appendStringInfoString(str, "'\"'\"'");
		else
			appendStringInfoChar(str, *p);
	}
	appendStringInfoChar(str, '\'');
#else							/* WIN32 */

	/*
	 * A Windows system() argument experiences two layers of interpretation.
	 * First, cmd.exe interprets the string.  Its behavior is undocumented,
	 * but a caret escapes any byte except LF or CR that would otherwise have
	 * special meaning.  Handling of a caret before LF or CR differs between
	 * "cmd.exe /c" and other modes, and it is unusable here.
	 *
	 * Second, the new process parses its command line to construct argv (see
	 * https://msdn.microsoft.com/en-us/library/17w5ykft.aspx).  This treats
	 * backslash-double quote sequences specially.
	 */
	appendStringInfoString(str, "^\"");
	for (p = s; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			ok = false;
			continue;
		}

		/* Change N backslashes before a double quote to 2N+1 backslashes. */
		if (*p == '"')
		{
			while (backslash_run_length)
			{
				appendStringInfoString(str, "^\\");
				backslash_run_length--;
			}
			appendStringInfoString(str, "^\\");
		}
		else if (*p == '\\')
			backslash_run_length++;
		else
			backslash_run_length = 0;

		/*
		 * Decline to caret-escape the most mundane characters, to ease
		 * debugging and lest we approach the command length limit.
		 */
		if (!((*p >= 'a' && *p <= 'z') ||
			  (*p >= 'A' && *p <= 'Z') ||
			  (*p >= '0' && *p <= '9')))
			appendStringInfoChar(str, '^');
		appendStringInfoChar(str, *p);
	}

	/*
	 * Change N backslashes at end of argument to 2N backslashes, because they
	 * precede the double quote that terminates the argument.
	 */
	while (backslash_run_length)
	{
		appendStringInfoString(str, "^\\");
		backslash_run_length--;
	}
	appendStringInfoString(str, "^\"");
#endif							/* WIN32 */

	return ok;
}

/*
 * appendBinaryStringInfo
 *
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary. Ensures that a trailing null byte is present.
 */
void
appendBinaryStringInfo(StringInfo str, const void *data, int datalen)
{
	Assert(str != NULL);

	/* Make more room if needed */
	enlargeStringInfo(str, datalen);

	/* OK, append the data */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;

	/*
	 * Keep a trailing null in place, even though it's probably useless for
	 * binary data.  (Some callers are dealing with text but call this because
	 * their input isn't null-terminated.)
	 */
	str->data[str->len] = '\0';
}

/*
 * appendBinaryStringInfoNT
 *
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary. Does not ensure a trailing null-byte exists.
 */
void
appendBinaryStringInfoNT(StringInfo str, const void *data, int datalen)
{
	Assert(str != NULL);

	/* Make more room if needed */
	enlargeStringInfo(str, datalen);

	/* OK, append the data */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;
}

/*
 * enlargeStringInfo
 *
 * Make sure there is enough space for 'needed' more bytes
 * ('needed' does not include the terminating null).
 *
 * External callers usually need not concern themselves with this, since
 * all stringinfo.c routines do it automatically.  However, if a caller
 * knows that a StringInfo will eventually become X bytes large, it
 * can save some palloc overhead by enlarging the buffer before starting
 * to store data in it.
 *
 * NB: In the backend, because we use repalloc() to enlarge the buffer, the
 * string buffer will remain allocated in the same memory context that was
 * current when initStringInfo was called, even if another context is now
 * current.  This is the desired and indeed critical behavior!
 */
void
enlargeStringInfo(StringInfo str, int needed)
{
	int			newlen;

	/* validate this is not a read-only StringInfo */
	Assert(str->maxlen != 0);

	/*
	 * Guard against out-of-range "needed" values.  Without this, we can get
	 * an overflow or infinite loop in the following.
	 */
	if (needed < 0)				/* should not happen */
	{
#ifndef FRONTEND
		elog(ERROR, "invalid string enlargement request size: %d", needed);
#else
		fprintf(stderr, "invalid string enlargement request size: %d\n", needed);
		exit(EXIT_FAILURE);
#endif
	}
	if (((Size) needed) >= (MaxAllocSize - (Size) str->len))
	{
#ifndef FRONTEND
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string buffer exceeds maximum allowed length (%zu bytes)", MaxAllocSize),
				 errdetail("Cannot enlarge string buffer containing %d bytes by %d more bytes.",
						   str->len, needed)));
#else
		fprintf(stderr,
				_("string buffer exceeds maximum allowed length (%zu bytes)\n\nCannot enlarge string buffer containing %d bytes by %d more bytes.\n"),
				MaxAllocSize, str->len, needed);
		exit(EXIT_FAILURE);
#endif
	}

	needed += str->len + 1;		/* total space required now */

	/* Because of the above test, we now have needed <= MaxAllocSize */

	if (needed <= str->maxlen)
		return;					/* got enough space already */

	/*
	 * We don't want to allocate just a little more space with each append;
	 * for efficiency, double the buffer size each time it overflows.
	 * Actually, we might need to more than double it if 'needed' is big...
	 */
	newlen = 2 * str->maxlen;
	while (needed > newlen)
		newlen = 2 * newlen;

	/*
	 * Clamp to MaxAllocSize in case we went past it.  Note we are assuming
	 * here that MaxAllocSize <= INT_MAX/2, else the above loop could
	 * overflow.  We will still have newlen >= needed.
	 */
	if (newlen > (int) MaxAllocSize)
		newlen = (int) MaxAllocSize;

	str->data = (char *) repalloc(str->data, newlen);

	str->maxlen = newlen;
}

/*
 * destroyStringInfo
 *
 * Frees a StringInfo and its buffer (opposite of makeStringInfo()).
 * This must only be called on palloc'd StringInfos.
 */
void
destroyStringInfo(StringInfo str)
{
	/* don't allow destroys of read-only StringInfos */
	Assert(str->maxlen != 0);

	pfree(str->data);
	pfree(str);
}
