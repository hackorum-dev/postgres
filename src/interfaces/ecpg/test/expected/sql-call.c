/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "call.pgc"
#include <stdio.h>
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

#line 4 "call.pgc"


#line 1 "regression.h"






#line 5 "call.pgc"


int
main(void)
{
   /* exec sql begin declare section */
        
        
        
        
   
#line 11 "call.pgc"
 int hv1 = 10 ;
 
#line 12 "call.pgc"
 int hv2 = 20 ;
 
#line 13 "call.pgc"
 int ind1 = 0 ;
 
#line 14 "call.pgc"
 int ind2 = 0 ;
/* exec sql end declare section */
#line 15 "call.pgc"


   /* exec sql whenever sqlerror  do sqlprint ( ) ; */
#line 17 "call.pgc"

   { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 18 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 18 "call.pgc"
;

   /* Start a new transaction. */
   { ECPGtrans(__LINE__, NULL, "begin transaction");
#line 21 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 21 "call.pgc"


   /* Create test tables. */
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table t1 ( a int , b int )", ECPGt_EOIT, ECPGt_EORT);
#line 24 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 24 "call.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table t2 ( a int , b int )", ECPGt_EOIT, ECPGt_EORT);
#line 25 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 25 "call.pgc"


   /* Insert some data into test tables created above. */
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 values ( 10 , 100 ) , ( 30 , 300 ) , ( 50 , 500 )", ECPGt_EOIT, ECPGt_EORT);
#line 28 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 28 "call.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t2 values ( 20 , 200 ) , ( 40 , 400 ) , ( 60 , 600 )", ECPGt_EOIT, ECPGt_EORT);
#line 29 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 29 "call.pgc"


   /* Create stored procedure1 with INOUT params. */
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create or replace procedure sp1 ( inout v1 int , inout v2 int ) as $$\
     begin\
     v1 := (select b from t1 where a = v1);\
     v2 := (select b from t2 where a = v2);\
     end; $$ language plpgsql", ECPGt_EOIT, ECPGt_EORT);
#line 38 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 38 "call.pgc"


   /* Create stored procedure2 with IN and INOUT params. */
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create or replace procedure sp2 ( in v1 int , inout v2 int ) as $$\
     declare\
     v3 int := v2;\
     begin\
     v2 := (select b from t1 where a = v1);\
     v3 := (select b from t2 where a = v3);\
     end; $$ language plpgsql", ECPGt_EOIT, ECPGt_EORT);
#line 49 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 49 "call.pgc"


   /* Call stored procedure1 and print it's output. */
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "call sp1 ( $1  , $2  )", 
	ECPGt_int,&(hv1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(hv2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(hv1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(hv2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 52 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 52 "call.pgc"


   printf("Stored procedure1 output: hv1 = %d, hv2 = %d\n", hv1, hv2);

   /* Call stored procedure1 with indicator variables and print it's output. */
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "call sp1 ( $1  , $2  )", 
	ECPGt_int,&(hv1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(hv2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(hv1),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(ind1),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(hv2),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(ind2),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 57 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 57 "call.pgc"


   printf("\nStored procedure1 output with indicator variables.\n");
   printf("Indicator variables ind1 and ind2 must hold negative values indicating that server returned NULL values this time.\n");
   printf("\nhv1 = %d ind1 = %d, hv2 = %d ind2 = %d\n", hv1, ind1, hv2, ind2);

   /* Reset the value of hv1 and hv2 before calling stored procedure sp2. */
   hv1 = 30;
   hv2 = 40;

   /* Call stored procedure2 and print it's output. */
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "call sp2 ( $1  , $2  )", 
	ECPGt_int,&(hv1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(hv2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(hv2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 68 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 68 "call.pgc"


   printf("\nStored procedure2 output: hv2 = %d\n", hv2);

   { ECPGtrans(__LINE__, NULL, "rollback");
#line 72 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 72 "call.pgc"


   { ECPGdisconnect(__LINE__, "CURRENT");
#line 74 "call.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 74 "call.pgc"


   return 0;
}
