/*-------------------------------------------------------------------------
 *
 * log_encoding.c
 *	  Encoding conversion for server logs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/log_encoding.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/memutils.h"

StaticAssertDecl(MAX_CONVERSION_GROWTH >= 4,
				 "log encoding escape expansion exceeds conversion growth");

static const char log_conversion_truncated[] = " [truncated during log conversion]";

/*
 * Convert at most dstlen bytes, exclusive of the NUL terminator.  Preserve
 * undecodable bytes using the same hex convention as pg_clean_ascii().
 * Avoid SQL conversion lookup, since we may be logging outside a transaction
 * or inside an error handler.
 *
 * A full destination buffer is possible on the allocation failure path.
 * Truncate only at a character or escape boundary.
 */
static void
convert_log_text(const char *src, int len, int encoding, char *dst, int dstlen,
				 bool had_newline)
{
	static const char hex[] = "0123456789abcdef";
	char	   *end = dst + dstlen;
	bool		utf8_or_unknown = encoding == PG_UTF8 || encoding == PG_SQL_ASCII;

	while (len > 0 && dst < end)
	{
		int			converted;
		int			charlen;

		if (utf8_or_unknown)
		{
			int			n = Min(len, end - dst);

			converted = pg_encoding_verifymbstr(PG_UTF8, src, n);
			if (converted > 0)
			{
				memcpy(dst, src, converted);
				dst += converted;
				src += converted;
				len -= converted;
				continue;
			}

			if (n < len && pg_encoding_verifymbchar(PG_UTF8, src, len) > n)
				break;
			charlen = 1;
		}
		else
		{
			int			n = Min(len, (end - dst) / MAX_CONVERSION_GROWTH);

			if (n == 0)
				break;
			converted = pg_local_to_utf8((const unsigned char *) src, n,
										 (unsigned char *) dst, encoding, true);
			if (converted > 0)
			{
				dst += strlen(dst);
				src += converted;
				len -= converted;
				continue;
			}

			charlen = Max(1, pg_encoding_verifymbchar(encoding, src, len));
			if (charlen > n)
				break;
		}

		if (charlen > (end - dst) / 4)
			break;
		len -= charlen;
		while (charlen-- > 0)
		{
			unsigned char byte = (unsigned char) *src++;

			*dst++ = '\\';
			*dst++ = 'x';
			*dst++ = hex[byte >> 4];
			*dst++ = hex[byte & 0x0f];
		}
	}

	if (len > 0)
	{
		memcpy(dst, log_conversion_truncated, sizeof(log_conversion_truncated) - 1);
		dst += sizeof(log_conversion_truncated) - 1;
		if (had_newline)
			*dst++ = '\n';
	}
	*dst = '\0';
}

/*
 * Return the original string, a string in the caller's stack buffer, or the
 * allocated string in buffer->allocated.  Do not discard the diagnostic if the
 * extra allocation fails, even when reporting out of memory.
 */
const char *
log_convert_text(const char *text, int len, int encoding,
				 LogEncodingBuffer *buffer)
{
	int			first;
	int			limit;
	uint64		size;

	buffer->allocated = NULL;
	if (Log_encoding != PG_UTF8)
		return text;

	if (len < 0)
		len = strlen(text);
	if (!PG_VALID_ENCODING(encoding))
		encoding = PG_SQL_ASCII;

	if (encoding == PG_UTF8 || encoding == PG_SQL_ASCII)
		first = pg_encoding_verifymbstr(PG_UTF8, text, len);
	else
	{
		for (first = 0; first < len && text[first] &&
			 !IS_HIGHBIT_SET(text[first]); first++)
			;
	}
	if (first == len)
		return text;

	/*
	 * Skip the heap for short messages.  Leave space for an escape per byte
	 * and for the truncation notice on the stack-buffer fallback path.
	 */
	size = (uint64) (len - first) * MAX_CONVERSION_GROWTH + first + 1;
	if (size > sizeof(buffer->data) && size <= MaxAllocSize &&
		CurrentMemoryContext != NULL)
		buffer->allocated = palloc_extended(size, MCXT_ALLOC_NO_OOM);

	if (size <= sizeof(buffer->data) || buffer->allocated)
	{
		char	   *dst = buffer->allocated ? buffer->allocated : buffer->data;

		memcpy(dst, text, first);
		convert_log_text(text +first, len - first, encoding, dst + first,
						 size - first - 1, false);
		return dst;
	}

	/* Reserve space for the truncation notice, a newline and the NUL. */
	limit = sizeof(buffer->data) - sizeof(log_conversion_truncated) - 1;
	first = Min(first, limit);
	if (encoding == PG_UTF8 || encoding == PG_SQL_ASCII)
		first = pg_encoding_mbcliplen(PG_UTF8, text, first, first);
	memcpy(buffer->data, text, first);
	convert_log_text(text +first, len - first, encoding, buffer->data + first,
					 limit - first, text[len - 1] == '\n');
	return buffer->data;
}
