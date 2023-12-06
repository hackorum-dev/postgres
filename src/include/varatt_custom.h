/*-------------------------------------------------------------------------
 *
 * varatt_custom.h
 *	  CUSTOM Toast Pointer definition and macros
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/varatt_custom.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VARATT_CUSTOM_H
#define VARATT_CUSTOM_H

#include "postgres.h"
#include "varatt.h"

typedef struct uint32align16
{
	uint16	hi;
	uint16	lo;
} uint32align16;

#define set_uint32align16(p, v)	\
	( \
		(p)->hi = (v) >> 16, \
		(p)->lo = (v) & 0xffff \
	)

#define get_uint32align16(p)	\
	(((uint32)((p)->hi)) << 16 | ((uint32)((p)->lo)))

/* varatt_custom uses 16bit aligment */
typedef struct varatt_custom
{
	uint32align16	va_tptrdatalen;	/* total size of toast pointer, < BLCKSZ */
	uint32align16	va_rawsize;		/* Original data size (includes header) */
	char		va_tptrdata[FLEXIBLE_ARRAY_MEMBER];	/* Custom data */
}			varatt_custom;

/* Custom Toast pointer */
#define VARATT_CUSTOM_GET_TOASTPOINTER(PTR) \
	((varatt_custom *) VARDATA_EXTERNAL(PTR))

#define VARATT_CUSTOM_GET_DATA_RAW_SIZE(PTR) \
	(get_uint32align16(&VARATT_CUSTOM_GET_TOASTPOINTER(PTR)->va_rawsize))

#define VARATT_CUSTOM_SET_DATA_RAW_SIZE(PTR, V) \
	(set_uint32align16(&VARATT_CUSTOM_GET_TOASTPOINTER(PTR)->va_rawsize, (V)))

#define VARATT_CUSTOM_GET_DATA_SIZE(PTR) \
	(get_uint32align16(&VARATT_CUSTOM_GET_TOASTPOINTER(PTR)->va_tptrdatalen))

#define VARATT_CUSTOM_SET_DATA_SIZE(PTR, V) \
	(set_uint32align16(&VARATT_CUSTOM_GET_TOASTPOINTER(PTR)->va_tptrdatalen, (V)))

#define VARATT_CUSTOM_GET_DATA(PTR) \
	(VARATT_CUSTOM_GET_TOASTPOINTER(PTR)->va_tptrdata)

#define VARATT_CUSTOM_SIZE(datalen) \
	((Size) VARHDRSZ_EXTERNAL + offsetof(varatt_custom, va_tptrdata) + (datalen))

#define VARATT_CUSTOM_MAX_DATA_SIZE \
	(MaxAllocSize - VARATT_CUSTOM_SIZE(0))

#define VARSIZE_CUSTOM(PTR)	VARATT_CUSTOM_SIZE(VARATT_CUSTOM_GET_DATA_SIZE(PTR))

Size
toast_custom_datum_size(const void *ptr, ToastPtrSizeType sz_type)
{
	if (sz_type == TPTR_DATUM_SIZE ||
		sz_type == TPTR_STORAGE_SIZE)
		return offsetof(varatt_custom, va_tptrdatalen) + VARATT_CUSTOM_GET_DATA_SIZE(ptr);
	else if (sz_type == TPTR_RAW_SIZE)
		return VARATT_CUSTOM_GET_DATA_RAW_SIZE(ptr);
	else
		elog(ERROR, "invalid toast_custom_datum_size() size type");

	return 0; /* avoid warning */
}

#endif
