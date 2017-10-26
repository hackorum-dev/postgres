#ifndef ConvertUTF_INCLUDED
#define ConvertUTF_INCLUDED
#include <stddef.h>
#include "c.h"

/* define unicode character length by bytes */
#if defined(WIN32)
#define UNICODE_CH_LEN 2
#else
#define UNICODE_CH_LEN 4
#endif

typedef enum
{
	conversionOK, 		/* conversion successful */
	sourceExhausted,	/* partial character in source, but hit end */
	targetExhausted,	/* insuff. room in target for conversion */
	sourceIllegal,		/* source sequence is illegal/malformed */
	conversionUnsupported /* Don't support this type convert */
} ConversionResult;

/* This is for C++ and does no harm in C */
#ifdef __cplusplus
extern "C" {
#endif

bool utext_is_null(char *source);
long get_utext_length(char *source, unsigned int maxlen);

ConversionResult convert_func(char *from_code, char *to_code,
							  char *from, size_t from_len,
							  char *to, size_t to_len, int *real_len);

int get_converted_length(char *from_code, char *to_code, char *from, size_t from_len);
char *get_utext_encoding(void);
#ifdef __cplusplus
}
#endif

/* --------------------------------------------------------------------- */

#endif /* ConvertUTF_INCLUDED */
