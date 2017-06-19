/*-------------------------------------------------------------------------
 *
 * stringinfo.h
 *	  Declarations/definitions for "StringInfo" functions.
 *
 * StringInfo provides an indefinitely-extensible string data type.
 * It can be used to buffer either ordinary C strings (null-terminated text)
 * or arbitrary binary data.  All storage is allocated with palloc().
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/lib/stringinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRINGINFO_H
#define STRINGINFO_H

/*-------------------------
 * StringInfoData holds information about an extensible string.
 *		data	is the current buffer for the string (allocated with palloc).
 *		len		is the current string length.  There is guaranteed to be
 *				a terminating '\0' at data[len] unless the StringInfo was
 *				constructed from a caller-supplied buffer.
 *		maxlen	is the allocated size in bytes of 'data', i.e. the maximum
 *				string size (including the terminating '\0' char) that we can
 *				currently store in 'data' without having to reallocate
 *				more space.  We must always have maxlen > len unless the
 *				StringInfo is constant, in which case maxlen is -1.
 *		cursor	is initialized to zero by makeStringInfo, initStringInfo
 *				and initConstantStringInfo, but is not otherwise touched by the
 *				stringinfo.c routines.  Some routines use it to scan through a
 *				StringInfo.
 *-------------------------
 */
typedef struct StringInfoData
{
	char	   *data;
	int			len;
	int			maxlen;
	int			cursor;
} StringInfoData;

typedef StringInfoData *StringInfo;


/*------------------------
 * There are three ways to create a StringInfo object initially:
 *
 * StringInfo stringptr = makeStringInfo();
 *		Both the StringInfoData and the data buffer are palloc'd.
 *
 * StringInfoData string;
 * initStringInfo(&string);
 *		The data buffer is palloc'd but the StringInfoData is just local.
 *		This is the easiest approach for a StringInfo object that will
 *		only live as long as the current routine.
 *
 * char * buffer = ...;
 * Size buffer_length = ....;
 * StringInfoData string;
 * initConstantStringInfo(&string, buffer, buffer_length);
 * 		The data buffer is a constant that remains owned by the caller.
 * 		Resetting this StringInfo or appending to it is not allowed.
 * 		This form is mainly useful when the StringInfo is used as a
 * 		cursor over pre-existing data.
 *
 * To destroy a non-constant StringInfo, pfree() the data buffer, and then
 * pfree() the StringInfoData if it was palloc'd.  There's no special support
 * for this.  It is usually sufficient to just let the buffer be cleaned up
 * along with the rest of the memory context in which it was allocated.
 *
 * There is no need to destroy a constant StringInfo.
 *
 * NOTE: some routines build up a string using StringInfo, and then
 * release the StringInfoData but return the data string itself to their
 * caller.  At that point the data string looks like a plain palloc'd
 * string.
 *-------------------------
 */

/*------------------------
 * makeStringInfo
 * Create an empty 'StringInfoData' & return a pointer to it.
 */
extern StringInfo makeStringInfo(void);

/*------------------------
 * initStringInfo
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 */
extern void initStringInfo(StringInfo str);

/*------------------------
 * initConstantStringInfo
 * Initialize a StringInfoData struct (with previously undefined contents) to
 * wrap the supplied buffer and length. The passed buffer remains owned by the
 * caller. The resulting StringInfo is constant and cannot be appended to or
 * resized, but provides a cursor over the buffer.
 */
extern void
initConstantStringInfo(StringInfo str, const char *buf, Size length);

/*------------------------
 * resetStringInfo
 * Clears the current content of the StringInfo, if any. The
 * StringInfo remains valid.
 */
extern void resetStringInfo(StringInfo str);

/*------------------------
 * appendStringInfo
 * Format text data under the control of fmt (an sprintf-style format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 */
extern void appendStringInfo(StringInfo str, const char *fmt,...) pg_attribute_printf(2, 3);

/*------------------------
 * appendStringInfoVA
 * Attempt to format text data under the control of fmt (an sprintf-style
 * format string) and append it to whatever is already in str.  If successful
 * return zero; if not (because there's not enough space), return an estimate
 * of the space needed, without modifying str.  Typically the caller should
 * pass the return value to enlargeStringInfo() before trying again; see
 * appendStringInfo for standard usage pattern.
 */
extern int	appendStringInfoVA(StringInfo str, const char *fmt, va_list args) pg_attribute_printf(2, 0);

/*------------------------
 * appendStringInfoString
 * Append a null-terminated string to str.
 * Like appendStringInfo(str, "%s", s) but faster.
 */
extern void appendStringInfoString(StringInfo str, const char *s);

/*------------------------
 * appendStringInfoChar
 * Append a single byte to str.
 * Like appendStringInfo(str, "%c", ch) but much faster.
 */
extern void appendStringInfoChar(StringInfo str, char ch);

/*------------------------
 * appendStringInfoCharMacro
 * As above, but a macro for even more speed where it matters.
 * Caution: str argument will be evaluated multiple times.
 */
#define appendStringInfoCharMacro(str,ch) \
	(((str)->len + 1 >= (str)->maxlen) ? \
	 appendStringInfoChar(str, ch) : \
	 (void)((str)->data[(str)->len] = (ch), (str)->data[++(str)->len] = '\0'))

/*------------------------
 * appendStringInfoSpaces
 * Append a given number of spaces to str.
 */
extern void appendStringInfoSpaces(StringInfo str, int count);

/*------------------------
 * appendBinaryStringInfo
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary.
 */
extern void appendBinaryStringInfo(StringInfo str,
					   const char *data, int datalen);

/*------------------------
 * enlargeStringInfo
 * Make sure a StringInfo's buffer can hold at least 'needed' more bytes.
 */
extern void enlargeStringInfo(StringInfo str, int needed);

/*------------------------
 * stringInfoIsConstant
 * Returns true if the StringInfo points to an externally allocated constant
 * buffer per initConstantStringInfo(...)
 */
extern bool stringInfoIsConstant(StringInfo str);

#endif   /* STRINGINFO_H */
