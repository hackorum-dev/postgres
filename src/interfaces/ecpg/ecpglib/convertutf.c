/*
/* src/interfaces/ecpg/ecpglib/convertutf.c */

#include "convertutf.h"
#include "c.h"
#ifdef CVTUTF_DEBUG
#include <stdio.h>
#endif
#include <iconv.h>
#include <errno.h> 

#ifndef false
#define false	0
#endif

#ifndef true
#define true	1
#endif

static bool is_little_endian(void);

/* Return true if utext string is null*/
bool utext_is_null(char *source)
{
#if defined(WIN32)
	if(*(short int*)source == 0)
#else
	if(*(int*)source == 0)
#endif
		return true;

	return false;
}


/*****
 * Get the UTF32 or UTF16 string length by characters.
 * Four bytes UTF16 consider it as 2 characters.
 * maxlen means the max character length of the unicode string.
 * if maxlen = 0, then get the length until find the 0x0000.
 */
long get_utext_length(char *source, unsigned int maxlen)
{
	long len = 0;

	while(1)
	{
#if defined(WIN32)
		if ((short)(*source) == 0x00)
#else
		if ((int)(*source) == 0x00)
#endif
			break;

		len++;
		source += UNICODE_CH_LEN;

		if(maxlen && len>=maxlen)
			return maxlen;
	}

	return len;
}

ConversionResult
convert_func(char *from_code, char *to_code, char *from, size_t from_len, char *to, size_t to_len, int *real_len)
{
    iconv_t cd;
    int len = to_len;
    ConversionResult ret = conversionOK;

    //cd = iconv_open("UTF-16LE","SHIFT-JIS");
    cd = iconv_open(to_code,from_code);

    if((cd) == (iconv_t) -1)
        return conversionUnsupported;

    if(iconv(cd, &from, &from_len, &to, &to_len) == -1)
    {
		int num = 0;

#if defined(WIN32)
		num = GetLastError();
		if (num ==0 && to_len == 0 && from_len > 0)
			num = E2BIG;
#else
		num = errno;
#endif

    	if(num == E2BIG)
    		ret = targetExhausted;
    	else
    		ret =  sourceIllegal;
    }
    else
    {
    	if(real_len)
    		*real_len = (len - to_len);
    }

    iconv_close(cd);

    return ret;
}

/*
 * This function return the need converted length by bytes.
 */
int get_converted_length(char *from_code, char *to_code, char *from, size_t from_len)
{
	iconv_t cd;
	size_t to_len = from_len *4;
	int max_len;
	int	convert_ret;
    char buffer[512]={0};
    char *to = buffer;
    bool need_free = false;

    if (to_len > 512)
    {
    	to = (char*)malloc(to_len);
    	memset(to,0,to_len);
    	need_free = true;
    }

    max_len = to_len;

    cd = iconv_open(to_code,from_code);
    if((cd) == (iconv_t) -1)
        return conversionUnsupported;

    convert_ret = (int)iconv(cd, &from, &from_len, &to, &to_len);

    iconv_close(cd);

    if(need_free)
    	free(to);

    if(convert_ret==-1)
    	return convert_ret;

    return (max_len - to_len);
}

/*****
 * Return the unicode encoding name based on the platform
 */
char *
get_utext_encoding()
{
#if defined(WIN32)
	if(is_little_endian())
		return "UTF-16LE";
	else
		return "UTF-16BE";
#else
	if(is_little_endian())
		return "UTF32LE";
	else
		return "UTF32BE";
#endif
}

/*
 * Check which type of endianness format supported by the machine
 * Return:
 * 	True -- little endian
 * 	False -- big endian
 *
 */
static bool
is_little_endian()
{
	int n = 1;

	if(*(char *)&n == 1)
		return true;

	return false;
}
