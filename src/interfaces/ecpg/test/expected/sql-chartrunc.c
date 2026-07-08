/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "chartrunc.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "sqlca.h"
#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifndef PGDLLIMPORT
#if  defined(WIN32) || defined(__CYGWIN__)
#define PGDLLIMPORT __declspec (dllimport)
#else
#define PGDLLIMPORT
#endif							/* __CYGWIN__ */
#endif							/* PGDLLIMPORT */

#define SQLERRMC_LEN	150

#ifdef __cplusplus
extern "C"
{
#endif

struct sqlca_t
{
	char		sqlcaid[8];
	long		sqlabc;
	long		sqlcode;
	struct
	{
		int			sqlerrml;
		char		sqlerrmc[SQLERRMC_LEN];
	}			sqlerrm;
	char		sqlerrp[8];
	long		sqlerrd[6];
	/* Element 0: empty						*/
	/* 1: OID of processed tuple if applicable			*/
	/* 2: number of rows processed				*/
	/* after an INSERT, UPDATE or				*/
	/* DELETE statement					*/
	/* 3: empty						*/
	/* 4: empty						*/
	/* 5: empty						*/
	char		sqlwarn[8];
	/* Element 0: set to 'W' if at least one other is 'W'	*/
	/* 1: if 'W' at least one character string		*/
	/* value was truncated when it was			*/
	/* stored into a host variable.             */

	/*
	 * 2: if 'W' a (hopefully) non-fatal notice occurred
	 */	/* 3: empty */
	/* 4: empty						*/
	/* 5: empty						*/
	/* 6: empty						*/
	/* 7: empty						*/

	char		sqlstate[5];
};

struct sqlca_t *ECPGget_sqlca(void);

#ifndef POSTGRES_ECPG_INTERNAL
#define sqlca (*ECPGget_sqlca())
#endif

#ifdef __cplusplus
}
#endif

#endif

#line 5 "chartrunc.pgc"


#line 1 "regression.h"






#line 6 "chartrunc.pgc"


/*
 * Test fixed-size char[] and VARCHAR output around the buffer boundary in
 * regular mode.  char[] is printed with a precision because exact-fit and
 * truncated values are not NUL-terminated.
 */
int main(void)
{
	/* exec sql begin declare section */
				
				
				
				
	
#line 16 "chartrunc.pgc"
 char c1 [ 4 ] ;
 
#line 17 "chartrunc.pgc"
  struct varchar_1  { int len; char arr[ 4 ]; }  v1 ;
 
#line 18 "chartrunc.pgc"
 short c1_ind ;
 
#line 19 "chartrunc.pgc"
 short v1_ind ;
/* exec sql end declare section */
#line 20 "chartrunc.pgc"


	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); }
#line 22 "chartrunc.pgc"

	/* exec sql whenever sqlerror  stop ; */
#line 23 "chartrunc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table ecpg_chartrunc ( id int , val varchar ( 10 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 25 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 25 "chartrunc.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into ecpg_chartrunc values ( 1 , 'abc' )", ECPGt_EOIT, ECPGt_EORT);
#line 26 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 26 "chartrunc.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into ecpg_chartrunc values ( 2 , 'abcd' )", ECPGt_EOIT, ECPGt_EORT);
#line 27 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 27 "chartrunc.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into ecpg_chartrunc values ( 3 , 'abcde' )", ECPGt_EOIT, ECPGt_EORT);
#line 28 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 28 "chartrunc.pgc"


	/* declare cur cursor for select val , val from ecpg_chartrunc order by id */
#line 30 "chartrunc.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur cursor for select val , val from ecpg_chartrunc order by id", ECPGt_EOIT, ECPGt_EORT);
#line 31 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 31 "chartrunc.pgc"


	/* exec sql whenever not found  break ; */
#line 33 "chartrunc.pgc"

	for (;;)
	{
		memset(c1, 0, sizeof(c1));
		memset(v1.arr, 0, sizeof(v1.arr));
		v1.len = 0;
		c1_ind = v1_ind = 0;
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cur", ECPGt_EOIT, 
	ECPGt_char,(c1),(long)4,(long)1,(4)*sizeof(char), 
	ECPGt_short,&(c1_ind),(long)1,(long)1,sizeof(short), 
	ECPGt_varchar,&(v1),(long)4,(long)1,sizeof(struct varchar_1), 
	ECPGt_short,&(v1_ind),(long)1,(long)1,sizeof(short), ECPGt_EORT);
#line 40 "chartrunc.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 40 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 40 "chartrunc.pgc"

		printf("c1=\"%.4s\" c1_ind=%d warn=%c | v1.len=%d v1.arr=\"%.*s\" v1_ind=%d\n",
			   c1, c1_ind, (sqlca.sqlwarn[1] == 'W') ? 'W' : '-',
			   v1.len, v1.len, v1.arr, v1_ind);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur", ECPGt_EOIT, ECPGt_EORT);
#line 46 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 46 "chartrunc.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table ecpg_chartrunc", ECPGt_EOIT, ECPGt_EORT);
#line 47 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 47 "chartrunc.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");
#line 48 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 48 "chartrunc.pgc"


	{ ECPGdisconnect(__LINE__, "ALL");
#line 50 "chartrunc.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 50 "chartrunc.pgc"


	return 0;
}
