/*-------------------------------------------------------------------------
 *
 * win32setlocale.c
 *		Wrapper to work around bugs in Windows setlocale() implementation
 *
 * Copyright (c) 2011-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32setlocale.c
 *
 *
 * The setlocale() function in Windows is broken in two ways. First, it
 * has a problem with locale names that have a dot in the country name. For
 * example:
 *
 * "Chinese (Traditional)_Hong Kong S.A.R..950"
 *
 * For some reason, setlocale() doesn't accept that as argument, even though
 * setlocale(LC_ALL, NULL) returns exactly that. Fortunately, it accepts
 * various alternative names for such countries, so to work around the broken
 * setlocale() function, we map the troublemaking locale names to accepted
 * aliases, before calling setlocale().
 *
 * The second problem is that the locale name for "Norwegian (Bokm&aring;l)"
 * contains a non-ASCII character. That's problematic, because it's not clear
 * what encoding the locale name itself is supposed to be in, when you
 * haven't yet set a locale. Also, it causes problems when the cluster
 * contains databases with different encodings, as the locale name is stored
 * in the pg_database system catalog. To work around that, when setlocale()
 * returns that locale name, map it to a pure-ASCII alias for the same
 * locale.
 *
 * These workarounds are complicated by the fact that these long-form locale
 * names aren't particularly static across Windows versions; punctuation and
 * spacing, for example, can vary.  To ensure we match when we should match,
 * ignore everything but ASCII letters in the locale name.  (This also eases
 * recognizing Bokm&aring;l.)
 *-------------------------------------------------------------------------
 */

#include "c.h"

#undef setlocale

struct locale_map
{
	/*
	 * String in locale name to replace.  While matching, we consider only
	 * plain ASCII letters, and the match is case-insensitive.
	 */
	const char *locale_name;	/* locale name to search for */

	const char *replacement;	/* string to replace the match with */

	/*
	 * If this is true, copy any code page specification (trailing .NNNN) from
	 * the source locale name.
	 */
	bool		copy_code_page;
};

/*
 * Mappings applied before calling setlocale(), to the argument.
 */
static const struct locale_map locale_map_argument[] = {
	/*
	 * "HKG" is listed here:
	 * https://docs.microsoft.com/en-us/cpp/c-runtime-library/country-region-strings
	 * (Country/Region Strings).
	 *
	 * "ARE" is the ISO-3166 three-letter code for U.A.E. It is not on the
	 * above list, but seems to work anyway.
	 */
	{"Hong Kong S.A.R.", "HKG", true},
	{"U.A.E.", "ARE", true},

	/*
	 * The ISO-3166 country code for Macau S.A.R. is MAC, but Windows doesn't
	 * seem to recognize that. And Macau isn't listed in the table of accepted
	 * abbreviations linked above. Fortunately, "ZHM" seems to be accepted as
	 * an alias for "Chinese (Traditional)_Macau S.A.R..950". I'm not sure
	 * where "ZHM" comes from, must be some legacy naming scheme. But hey, it
	 * works.
	 *
	 * Note that unlike HKG and ARE, ZHM is an alias for the *whole* locale
	 * name, not just the country part, so we suppress any code page spec.
	 *
	 * Some versions of Windows spell it "Macau", others "Macao".
	 */
	{"Chinese (Traditional)_Macau S.A.R..950", "ZHM", false},
	{"Chinese_Macau S.A.R..950", "ZHM", false},
	{"Chinese (Traditional)_Macao S.A.R..950", "ZHM", false},
	{"Chinese_Macao S.A.R..950", "ZHM", false},
	{NULL, NULL, false}
};

/*
 * Mappings applied after calling setlocale(), to its return value.
 */
static const struct locale_map locale_map_result[] = {
	/*
	 * "Norwegian (Bokm&aring;l)" locale name contains the a-ring character.
	 * Map it to a pure-ASCII alias.
	 *
	 * It's not clear what encoding setlocale() uses when it returns the
	 * locale name, but since the search will ignore non-ASCII characters, we
	 * can just leave &aring; out of the match string.
	 */
	{"Norwegian Bokml Norway", "Norwegian_Norway", true},
	{NULL, NULL, false}
};

#define MAX_LOCALE_NAME_LEN		100

static const char *
map_locale(const struct locale_map *map, const char *locale)
{
	static char aliasbuf[MAX_LOCALE_NAME_LEN];
	int			i;

	/* Check if the locale name matches any of the problematic ones. */
	for (i = 0; map[i].locale_name != NULL; i++)
	{
		const char *needle = map[i].locale_name;
		const char *replacement = map[i].replacement;
		bool		match = true;
		const char *p1,
				   *p2;
		const char *codepage;
		int			replacementlen;
		int			cplen;

#define ASCII_LETTERS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

		p1 = locale;
		p2 = needle;
		for (;;)
		{
			/* Ignore characters that aren't ASCII letters */
			while (*p1 && strchr(ASCII_LETTERS, (unsigned char) *p1) == NULL)
				p1++;

			while (*p2 && strchr(ASCII_LETTERS, (unsigned char) *p2) == NULL)
				p2++;

			/* Must match case-insensitively */
			if (*p1 && *p2)
			{
				if (pg_toupper(*p1) != pg_toupper(*p2))
				{
					match = false;
					break;
				}
				p1++, p2++;
			}
			else
			{
				if (*p1 || *p2)
					match = false;	/* one is longer */
				break;
			}
		}

		if (!match)
			continue;

		/* Found a match.  Should we include the codepage spec, if any? */
		if (map[i].copy_code_page)
		{
			codepage = strrchr(locale, '.');
			if (!(codepage && codepage[1] &&
				  strspn(codepage + 1, "0123456789") == strlen(codepage + 1)))
				codepage = NULL;
		}
		else
			codepage = NULL;

		/* check that the result fits in the static buffer */
		replacementlen = strlen(replacement);
		cplen = (codepage ? strlen(codepage) : 0);
		if (replacementlen + cplen + 1 > MAX_LOCALE_NAME_LEN)
			break;				/* treat as no-match */

		memcpy(&aliasbuf[0], replacement, replacementlen + 1);
		if (codepage)
			memcpy(&aliasbuf[replacementlen], codepage, cplen + 1);

		return aliasbuf;
	}

	/* no match, just return the original string */
	return locale;
}

char *
pgwin32_setlocale(int category, const char *locale)
{
	const char *argument;
	char	   *result;

	if (locale == NULL)
		argument = NULL;
	else
		argument = map_locale(locale_map_argument, locale);

	/* Call the real setlocale() function */
	result = setlocale(category, argument);

	/*
	 * setlocale() is specified to return a "char *" that the caller is
	 * forbidden to modify, so casting away the "const" is innocuous.
	 */
	if (result)
		result = unconstify(char *, map_locale(locale_map_result, result));

	return result;
}
