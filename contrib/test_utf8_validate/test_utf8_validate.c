#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

#define MAX_STRING_SIZE 100000
#define MULTIPLIER 10000000

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(drive_utf8_validate);
Datum
drive_utf8_validate(PG_FUNCTION_ARGS)
{
	int string_size = PG_GETARG_INT32(0);
	static unsigned char * test_string = NULL;
	static int (*verifystr)(const unsigned char *s, int len);

	verifystr = pg_wchar_table[PG_UTF8].mbverifystr;

	if (test_string == NULL){
		test_string = palloc0(MAX_STRING_SIZE + 1);
		for (int i = 0; i < MAX_STRING_SIZE; i++) test_string[i] = 'A';
		test_string[MAX_STRING_SIZE] = '\0';
	}

	if (string_size > 0){
		for (int i = 0; i < MULTIPLIER; i++){
			volatile int result = verifystr(test_string, string_size);
		(void) result;
		}
	}

	PG_RETURN_VOID();
}
