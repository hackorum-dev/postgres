/*-------------------------------------------------------------------------
 *
 * pgstrcasecmp.c
 *	   Portable case-independent comparisons and conversions.
 *
 * SQL99 specifies Unicode-aware case normalization, but for historical
 * reasons this was never fully supported. Just uses ASCII-only case
 * conversion.
 *
 * We also provide explcit ASCII-only case conversion functions, which can
 * be used to implement C/POSIX case folding semantics no matter what the
 * C library thinks the locale is.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/port/pgstrcasecmp.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <ctype.h>


/*
 * Case-independent comparison of two null-terminated strings.
 */
int
pg_strcasecmp(const char *s1, const char *s2)
{
	for (;;)
	{
		unsigned char ch1 = (unsigned char) *s1++;
		unsigned char ch2 = (unsigned char) *s2++;

		if (ch1 != ch2)
		{
			if (ch1 >= 'A' && ch1 <= 'Z')
				ch1 += 'a' - 'A';

			if (ch2 >= 'A' && ch2 <= 'Z')
				ch2 += 'a' - 'A';

			if (ch1 != ch2)
				return (int) ch1 - (int) ch2;
		}
		if (ch1 == 0)
			break;
	}
	return 0;
}

/*
 * Case-independent comparison of two not-necessarily-null-terminated strings.
 * At most n bytes will be examined from each string.
 */
int
pg_strncasecmp(const char *s1, const char *s2, size_t n)
{
	while (n-- > 0)
	{
		unsigned char ch1 = (unsigned char) *s1++;
		unsigned char ch2 = (unsigned char) *s2++;

		if (ch1 != ch2)
		{
			if (ch1 >= 'A' && ch1 <= 'Z')
				ch1 += 'a' - 'A';

			if (ch2 >= 'A' && ch2 <= 'Z')
				ch2 += 'a' - 'A';

			if (ch1 != ch2)
				return (int) ch1 - (int) ch2;
		}
		if (ch1 == 0)
			break;
	}
	return 0;
}

/*
 * Fold a character to upper case.
 *
 * Unlike some versions of toupper(), this is safe to apply to characters
 * that aren't lower case letters.
 */
unsigned char
pg_toupper(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'z')
		ch += 'A' - 'a';
	return ch;
}

/*
 * Fold a character to lower case.
 *
 * Unlike some versions of tolower(), this is safe to apply to characters
 * that aren't upper case letters.
 */
unsigned char
pg_tolower(unsigned char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		ch += 'a' - 'A';
	return ch;
}

/*
 * Fold a character to upper case, following C/POSIX locale rules.
 */
unsigned char
pg_ascii_toupper(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'z')
		ch += 'A' - 'a';
	return ch;
}

/*
 * Fold a character to lower case, following C/POSIX locale rules.
 */
unsigned char
pg_ascii_tolower(unsigned char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		ch += 'a' - 'A';
	return ch;
}
