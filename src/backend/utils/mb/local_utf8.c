/*-------------------------------------------------------------------------
 *
 * local_utf8.c
 *	  Direct conversions from PostgreSQL encodings to UTF8.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/local_utf8.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "mb/pg_wchar.h"

#include "gb18030.h"

#include "Unicode/euc_jp_to_utf8.map"
#include "Unicode/euc_cn_to_utf8.map"
#include "Unicode/euc_kr_to_utf8.map"
#include "Unicode/euc_tw_to_utf8.map"
#include "Unicode/euc_jis_2004_to_utf8.map"
#include "Unicode/iso8859_2_to_utf8.map"
#include "Unicode/iso8859_3_to_utf8.map"
#include "Unicode/iso8859_4_to_utf8.map"
#include "Unicode/iso8859_9_to_utf8.map"
#include "Unicode/iso8859_10_to_utf8.map"
#include "Unicode/iso8859_13_to_utf8.map"
#include "Unicode/iso8859_14_to_utf8.map"
#include "Unicode/iso8859_15_to_utf8.map"
#include "Unicode/iso8859_16_to_utf8.map"
#include "Unicode/win1256_to_utf8.map"
#include "Unicode/win1258_to_utf8.map"
#include "Unicode/win866_to_utf8.map"
#include "Unicode/win874_to_utf8.map"
#include "Unicode/koi8r_to_utf8.map"
#include "Unicode/win1251_to_utf8.map"
#include "Unicode/win1252_to_utf8.map"
#include "Unicode/iso8859_5_to_utf8.map"
#include "Unicode/iso8859_6_to_utf8.map"
#include "Unicode/iso8859_7_to_utf8.map"
#include "Unicode/iso8859_8_to_utf8.map"
#include "Unicode/win1250_to_utf8.map"
#include "Unicode/win1253_to_utf8.map"
#include "Unicode/win1254_to_utf8.map"
#include "Unicode/win1255_to_utf8.map"
#include "Unicode/win1257_to_utf8.map"
#include "Unicode/koi8u_to_utf8.map"
#include "Unicode/sjis_to_utf8.map"
#include "Unicode/big5_to_utf8.map"
#include "Unicode/gb18030_to_utf8.map"
#include "Unicode/gbk_to_utf8.map"
#include "Unicode/uhc_to_utf8.map"
#include "Unicode/johab_to_utf8.map"
#include "Unicode/shift_jis_2004_to_utf8.map"

/*
 * Keep the local-to-UTF8 tables in one place for SQL conversions and
 * callers that must work without access to the catalogs, such as logging.
 */
static const struct
{
	const pg_mb_radix_tree *map;
	const pg_local_to_utf_combined *combined;
	int			ncombined;
}			local_utf8_maps[_PG_LAST_ENCODING_] = {
	[PG_EUC_JP] = {&euc_jp_to_unicode_tree, NULL, 0},
	[PG_EUC_CN] = {&euc_cn_to_unicode_tree, NULL, 0},
	[PG_EUC_KR] = {&euc_kr_to_unicode_tree, NULL, 0},
	[PG_EUC_TW] = {&euc_tw_to_unicode_tree, NULL, 0},
	[PG_EUC_JIS_2004] = {&euc_jis_2004_to_unicode_tree,
	LUmapEUC_JIS_2004_combined, lengthof(LUmapEUC_JIS_2004_combined)},
	[PG_LATIN2] = {&iso8859_2_to_unicode_tree, NULL, 0},
	[PG_LATIN3] = {&iso8859_3_to_unicode_tree, NULL, 0},
	[PG_LATIN4] = {&iso8859_4_to_unicode_tree, NULL, 0},
	[PG_LATIN5] = {&iso8859_9_to_unicode_tree, NULL, 0},
	[PG_LATIN6] = {&iso8859_10_to_unicode_tree, NULL, 0},
	[PG_LATIN7] = {&iso8859_13_to_unicode_tree, NULL, 0},
	[PG_LATIN8] = {&iso8859_14_to_unicode_tree, NULL, 0},
	[PG_LATIN9] = {&iso8859_15_to_unicode_tree, NULL, 0},
	[PG_LATIN10] = {&iso8859_16_to_unicode_tree, NULL, 0},
	[PG_WIN1256] = {&win1256_to_unicode_tree, NULL, 0},
	[PG_WIN1258] = {&win1258_to_unicode_tree, NULL, 0},
	[PG_WIN866] = {&win866_to_unicode_tree, NULL, 0},
	[PG_WIN874] = {&win874_to_unicode_tree, NULL, 0},
	[PG_KOI8R] = {&koi8r_to_unicode_tree, NULL, 0},
	[PG_WIN1251] = {&win1251_to_unicode_tree, NULL, 0},
	[PG_WIN1252] = {&win1252_to_unicode_tree, NULL, 0},
	[PG_ISO_8859_5] = {&iso8859_5_to_unicode_tree, NULL, 0},
	[PG_ISO_8859_6] = {&iso8859_6_to_unicode_tree, NULL, 0},
	[PG_ISO_8859_7] = {&iso8859_7_to_unicode_tree, NULL, 0},
	[PG_ISO_8859_8] = {&iso8859_8_to_unicode_tree, NULL, 0},
	[PG_WIN1250] = {&win1250_to_unicode_tree, NULL, 0},
	[PG_WIN1253] = {&win1253_to_unicode_tree, NULL, 0},
	[PG_WIN1254] = {&win1254_to_unicode_tree, NULL, 0},
	[PG_WIN1255] = {&win1255_to_unicode_tree, NULL, 0},
	[PG_WIN1257] = {&win1257_to_unicode_tree, NULL, 0},
	[PG_KOI8U] = {&koi8u_to_unicode_tree, NULL, 0},
	[PG_SJIS] = {&sjis_to_unicode_tree, NULL, 0},
	[PG_BIG5] = {&big5_to_unicode_tree, NULL, 0},
	[PG_GBK] = {&gbk_to_unicode_tree, NULL, 0},
	[PG_UHC] = {&uhc_to_unicode_tree, NULL, 0},
	[PG_GB18030] = {&gb18030_to_unicode_tree, NULL, 0},
	[PG_JOHAB] = {&johab_to_unicode_tree, NULL, 0},
	[PG_SHIFT_JIS_2004] = {&shift_jis_2004_to_unicode_tree,
	LUmapSHIFT_JIS_2004_combined, lengthof(LUmapSHIFT_JIS_2004_combined)},
};

/*
 * LocalToUtf expects its callback to return UTF-8 bytes packed into a word.
 */
static uint32
unicode_to_utf8word(char32_t code)
{
	unsigned char utf8[4];
	int			len = unicode_utf8len(code);
	uint32		word = 0;

	unicode_to_utf8(code, utf8);
	for (int i = 0; i < len; i++)
		word = (word << 8) | utf8[i];

	return word;
}

/*
 * Perform mapping of GB18030 ranges to UTF8
 *
 * General description, and the range we need to convert for U+10000 and up:
 * https://htmlpreview.github.io/?https://github.com/unicode-org/icu-data/blob/main/charset/source/gb18030/gb18030.html
 *
 * Ranges up to U+FFFF:
 * https://github.com/unicode-org/icu-data/blob/main/charset/source/gb18030/ranges.txt
 *
 * All are ranges of 4-byte GB18030 codes.
 */
static uint32
conv_18030_to_utf8(uint32 code)
{
#define conv18030(minunicode, mincode, maxcode) \
	if (code >= mincode && code <= maxcode) \
		return unicode_to_utf8word(gb_linear(code) - gb_linear(mincode) + minunicode)

	conv18030(0x0452, 0x8130D330, 0x8136A531);
	conv18030(0x2643, 0x8137A839, 0x8138FD38);
	conv18030(0x361B, 0x8230A633, 0x8230F237);
	conv18030(0x3CE1, 0x8231D438, 0x8232AF32);
	conv18030(0x4160, 0x8232C937, 0x8232F837);
	conv18030(0x44D7, 0x8233A339, 0x8233C931);
	conv18030(0x478E, 0x8233E838, 0x82349638);
	conv18030(0x49B8, 0x8234A131, 0x8234E733);
	conv18030(0x9FA6, 0x82358F33, 0x8336C738);
	conv18030(0xE865, 0x8336D030, 0x84308534);
	conv18030(0xFA2A, 0x84309C38, 0x84318537);
	conv18030(0xFFE6, 0x8431A234, 0x8431A439);
	conv18030(0x10000, 0x90308130, 0xE3329A35);
	/* No mapping exists */
	return 0;
}

/*
 * Convert directly to UTF8 without fmgr or catalog lookups.  As with
 * LocalToUtf(), the result is NUL terminated and the return value is the
 * number of input bytes converted.
 */
int
pg_local_to_utf8(const unsigned char *src, int len, unsigned char *dest,
				 int encoding, bool noError)
{
	if (encoding == PG_LATIN1)
	{
		const unsigned char *start = src;

		for (; len > 0 && *src; len--, src++)
		{
			unicode_to_utf8(*src, dest);
			dest += IS_HIGHBIT_SET(*src) ? 2 : 1;
		}
		*dest = '\0';

		if (len > 0 && !noError)
			report_invalid_encoding(encoding, (const char *) src, len);
		return src - start;
	}

	if (PG_VALID_ENCODING(encoding) && local_utf8_maps[encoding].map)
		return LocalToUtf(src, len, dest, local_utf8_maps[encoding].map,
						  local_utf8_maps[encoding].combined,
						  local_utf8_maps[encoding].ncombined,
						  encoding == PG_GB18030 ? conv_18030_to_utf8 : NULL,
						  encoding, noError);

	if (!noError)
		elog(ERROR, "no direct UTF8 conversion for encoding %d", encoding);
	*dest = '\0';
	return 0;
}
