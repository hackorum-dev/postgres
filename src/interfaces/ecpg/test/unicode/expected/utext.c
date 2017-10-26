/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#define ECPG_ENABLE_UTEXT 1
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "utext.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>

#line 1 "./../regression.h"






#line 5 "utext.pgc"


#define test(msg) printf("\n%s\n",msg)
#define VAR_SIZE  20
#define ARRAY_SIZE 4
#define P_VAR_SIZE 22


/* Following is UTF8 and UTF16 characters mapping table */
/*太𠮷𠜱平洋𠱓大西洋印度洋北冰洋
0x592A 0xD842 0xDFB7 0xD841 0xDF31 0x5E73 0x6D0B 0xD843
0xDC53 0x5927 0x897F 0x6D0B 0x5370 0x5EA6 0x6D0B 0x5317
0x51B0 0x6D0B 0x0000,0x0000*/

/*足球篮球羽毛球乒乓球橄榄球棒球冰球
0x8DB3,0x7403,0x7BEE,0x7403,0x7FBD,0x6BDB,0x7403,0x4E52,
0x4E53,0x7403,0x6A44,0x6984,0x7403,0x68D2,0x7403,0x51B0,
0x7403,0x0000,0x0000,0x0000
*/

/*世界杯每隔四年就会举行一次每次𠲖个球队
0x4E16 0x754C 0x676F 0x6BCF 0x9694 0x56DB 0x5E74 0x5C31 
0x4F1A 0x4E3E 0x884C 0x4E00 0x6B21 0x6BCF 0x6B21 0xD843 
0xDC96 0x4E2A 0x7403 0x961F
*/

/* 亚洲欧洲非洲大洋洲北美洲南美洲南极洲没有北极洲
0x4E9A,0x6D32,0x6B27,0x6D32,0x975E,0x6D32,0x5927,0x6D0B,
0x6D32,0x5317,0x7F8E,0x6D32,0x5357,0x7F8E,0x6D32,0x5357,
0x6781,0x6D32,0x6CA1,0x6709,0x5317,0x6781,0x6D32
*/
/* 太𠮷
0x592A  0xD842  0xDFB7
*/
void init_var(void);
void test_var_1(void);
void test_var_2(void);
void test_var_3(void);
void test_var_4(void);
void test_var_5(void);
void test_var_6(void);
void test_var_7(void);
void test_var_8(void);
void test_var_9(void);
void test_var_10(void);
void test_var_11(void);
void test_var_12(void);
void test_var_13(void);
void test_array_1(void);
void test_array_2(void);
void test_array_3(void);
void test_array_4(void);
void test_array_5(void);
void test_array_6(void);
void test_array_7(void);
void test_array_8(void);
void test_array_9(void);
void test_array_10(void);
void test_array_11(void);
void test_array_12(void);
void test_array_13(void);
void test_pvar_1(void);
void test_pvar_2(void);
void test_pvar_3(void);
void test_pvar_4(void);
void test_pvar_5(void);
void test_pvar_6(void);
void test_pvar_7(void);
void test_pvar_8(void);
void test_pvar_9(void);
void test_pvar_10(void);
void test_pvar_11(void);
void test_pvar_12(void);
void test_pvar_13(void);
void test_buckinsert_1(void);

void test_all(void);

void print_utext(utext *utext_var);
void print_utext_ind(int utext_var_ind);
void print_utext_size(utext *utext_var,int size);
void print_array(void);
void print_array_with_index(int index);
int test_init(void);
void test_finish(void);
void init_table_value(void);
   
/* exec sql begin declare section */
           
           
			
	       
	     
	    
	
		
	   
		  
		
	     

	
			  
	     
	

#line 93 "utext.pgc"
 int utext_var_size = 20 ;
 
#line 94 "utext.pgc"
 int utext_array_size = 4 ;
 
#line 95 "utext.pgc"
 int count = 0 ;
 
#line 96 "utext.pgc"
 int total_tuples = 0 ;
 
#line 97 "utext.pgc"
 int i ;
 
#line 98 "utext.pgc"
 char char_var [ 10 ] = { 0xF0 , 0x90 , 0x90 , 0xB7 } ;
 
#line 100 "utext.pgc"
 utext utext_var [ VAR_SIZE ] ;
 
#line 101 "utext.pgc"
 utext utext_array [ ARRAY_SIZE ] [ VAR_SIZE ] ;
 
#line 102 "utext.pgc"
 utext utext_input_var [ 20 ] = { 0x592a , 0xd842 , 0xdfb7 , 0x0000 } ;
 
#line 103 "utext.pgc"
 utext utext_input_var2 [] = L"太𠮷" ;
 
#line 104 "utext.pgc"
 utext * p_utext_var = NULL ;
 
#line 107 "utext.pgc"
 int utext_var_ind ;
 
#line 108 "utext.pgc"
 int count_array [ 4 ] = { 1 , 2 , 3 , 4 } ;
/* exec sql end declare section */
#line 110 "utext.pgc"


void print_utext(utext *utext_var)
{
    int i;
    printf("======print utext_var content======\n");    
	for(i=0; i<VAR_SIZE; i++)
	{
	    printf ("0x%04X  ", utext_var[i]);
	    if(i>6 && (i+1)%8==0)
	        printf("\n");
	}
    printf("\n======End utext_var content======\n");
}

void print_utext_size(utext *utext_var,int size)
{
    int i;
    printf("======print utext_var content======\n");    
	for(i=0; i<size; i++)
	{
	    printf ("0x%04X  ", utext_var[i]);
	    if(i>6 && (i+1)%8==0)
	        printf("\n");
	}
    printf("\n======End utext_var content======\n");
}

void print_utext_ind(int utext_var_ind)
{
	printf("utext_var_ind = %d\n",utext_var_ind);
}

void print_array()
{
    int i,j;
    
	for(i=0; i<ARRAY_SIZE; i++)
	{
        printf ("---->array[%d]:\n", i);
        
	    for(j=0; j<VAR_SIZE; j++)
	    {
	        printf ("0x%04X  ", utext_array[i][j]);
	        if(j>6 && (j+1)%8==0)
	            printf("\n");
	    }
	    printf("\n");
	}

	printf("\n");
}

int test_init()
{
    p_utext_var = (utext*)malloc(P_VAR_SIZE*sizeof(utext));
    if(!p_utext_var)
    {
        printf("Error: failed allco memory for p_utext_var. \n");
        return -1;
    }
    
    
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 173 "utext.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);}
#line 175 "utext.pgc"

	/* exec sql whenever sql_warning  sqlprint ; */
#line 176 "utext.pgc"

	/* exec sql whenever sqlerror  sqlprint ; */
#line 177 "utext.pgc"



    //initialization of test table
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table if not exists tb1 ( Item varchar , count integer )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 181 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 181 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 181 "utext.pgc"

	
	init_table_value();
	
	return 0;
}

void test_finish()
{
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table tb1", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 190 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 190 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 190 "utext.pgc"

	{ ECPGdisconnect(__LINE__, "ALL");
#line 191 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 191 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 191 "utext.pgc"

}

void init_table_value()
{
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate tb1", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 196 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 196 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 196 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋' , 1 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 198 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 198 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 198 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '足球篮球羽毛球乒乓球橄榄球棒球冰球' , 2 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 199 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 199 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 199 "utext.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '世界杯每隔四年就会举行一次每次𠲖个球队' , 3 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 200 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 200 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 200 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '亚洲欧洲非洲大洋洲北美洲南美洲南极洲没有北极洲' , 4 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 201 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 201 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 201 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 5 )", 
	ECPGt_char,(char_var),(long)10,(long)1,(10)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 202 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 202 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 202 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '𠮷' , 6 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 203 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 203 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 203 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '太𠮷' , 7 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 204 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 204 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 204 "utext.pgc"

}

void init_var()
{
	count = 0;
    memset(utext_var,'a',sizeof(utext_var));
    memset(utext_array,'a',sizeof(utext_array));
    memset(p_utext_var,'a',P_VAR_SIZE*sizeof(utext));
}

//simple select into utext var
void test_var_1()
{
    test("test_var_1: simple select into utext var");
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 221 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 221 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 221 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 2", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 226 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 226 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 226 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 231 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 231 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 231 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 4", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 236 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 236 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 236 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
}


//simple select using utext var
void test_var_2()
{
    test("test_var_2: simple select using utext var");
    init_var();
    
    count = 0;
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 250 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 250 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 250 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 251 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 251 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 251 "utext.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	count = 0;
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 255 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 255 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 255 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 256 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 256 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 256 "utext.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using utext var
void test_var_3()
{
    test("test_var_3: simple update using utext var");
    init_var();

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 266 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 266 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 266 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 2", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 267 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 267 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 267 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 3", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 268 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 268 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 268 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 269 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 269 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 269 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_table_value();
}

//simple delete using utext var
void test_var_4()
{
    test("test_var_4 : simple delete using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 281 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 281 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 281 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 282 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 282 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 282 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 283 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 283 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 283 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 286 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 286 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 286 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 287 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 287 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 287 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 288 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 288 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 288 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using utext var
void test_var_5()
{
    test("test_var_5 : simple insert using utext");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 300 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 300 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 300 "utext.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 301 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 301 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 301 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 302 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 302 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 302 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
    
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 306 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 306 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 306 "utext.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 307 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 307 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 307 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 308 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 308 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 308 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into utext var
void test_var_6()
{
    test("test_var_6 : prepared select into utext var");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=?");
#line 320 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 320 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 320 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 322 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 322 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 322 "utext.pgc"

	print_utext(utext_var);
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 325 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 325 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 325 "utext.pgc"

	print_utext(utext_var);

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 328 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 328 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 328 "utext.pgc"
 
}

//prepared select using utext var
void test_var_7()
{
    test("test_var_7 : prepared select using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 337 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 337 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 337 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 339 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 339 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 339 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 340 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 340 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 340 "utext.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 343 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 343 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 343 "utext.pgc"

}

//prepared update using utext var
void test_var_8()
{
    test("test_var_8 : prepared update using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 352 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 352 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 352 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 354 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 354 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 354 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 355 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 355 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 355 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 356 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 356 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 356 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 357 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 357 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 357 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 360 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 360 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 360 "utext.pgc"

	
	init_table_value();
}

//prepared delete using utext var
void test_var_9()
{
    test("test_var_9 : prepared delete using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 371 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 371 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 371 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 373 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 373 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 373 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 374 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 374 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 374 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 375 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 375 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 375 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 378 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 378 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 378 "utext.pgc"

	
	init_table_value();
}

//prepared insert using utext var
void test_var_10()
{
    test("test_var_10 : prepared insert using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 389 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 389 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 389 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, 13)");
#line 391 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 391 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 391 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 392 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 392 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 392 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 393 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 393 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 393 "utext.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 396 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 396 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 396 "utext.pgc"

	
	init_table_value();
}

//Open cursor using utext var
void test_var_11()
{
    test("test_var_11 : Open cursor using utext var");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 407 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 407 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 407 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 408 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 408 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 408 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 410 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 410 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 410 "utext.pgc"

	/* declare cursor_var_11 cursor for $1 */
#line 411 "utext.pgc"

	{ ECPGopen("cursor_var_11", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_var_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 412 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 412 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 412 "utext.pgc"

	{ ECPGfetch("cursor_var_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_var_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 413 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 413 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 413 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGclose("cursor_var_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_var_11", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 415 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 415 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 415 "utext.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 416 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 416 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 416 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 417 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 417 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 417 "utext.pgc"

}

//Fecth cursor into utext var
void test_var_12()
{
    test("test_var_12 : Fecth cursor into utext var");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 426 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 426 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 426 "utext.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=1");
#line 427 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 427 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 427 "utext.pgc"

	/* declare cursor_var_12 cursor for $1 */
#line 428 "utext.pgc"

	{ ECPGopen("cursor_var_12", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_var_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 429 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 429 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 429 "utext.pgc"

	{ ECPGfetch("cursor_var_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_var_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 430 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 430 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 430 "utext.pgc"

	{ ECPGclose("cursor_var_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_var_12", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 431 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 431 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 431 "utext.pgc"

	
	print_utext(utext_var);
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 435 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 435 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 435 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 436 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 436 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 436 "utext.pgc"

}

//simple insert utext var with L string inited
void test_var_13()
{
/* exec sql begin declare section */
		

#line 443 "utext.pgc"
 utext utext_local_var [] = _T ( "太𠮷" ) ;
/* exec sql end declare section */
#line 444 "utext.pgc"

    test("test_var_13 : simple insert utext var with L string inited");
	init_var();

	printf("---utext_local_var---\n");
	
	for(i=0;i<sizeof(utext_local_var)/2;i++)
		printf("0x%04X ",utext_local_var[i]);
		
	printf("\n---end: utext_local_var---\n");
	
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 16 )", 
	ECPGt_utext,(utext_local_var),(long)sizeof(_T ( "太𠮷" ))/2,(long)1,(sizeof(_T ( "太𠮷" ))/2)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 455 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 455 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 455 "utext.pgc"

    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 16", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 457 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 457 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 457 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
    init_table_value();
}

//simple select into utext array 
void test_array_1()
{
    test("test_array_1 : simple select into utext array ");
    init_var();
    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 4", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 470 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 470 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 470 "utext.pgc"


	print_array();
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 475 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 475 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 475 "utext.pgc"
 
	print_array();
	init_var();
}

//simple select using utext array
void test_array_2()
{
    test("test_array_2 : simple select using array");
	init_var();
	    
 	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 486 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 486 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 486 "utext.pgc"
 

	count = 0;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 489 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 489 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 489 "utext.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	count = 0;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 493 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 493 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 493 "utext.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using utext array
void test_array_3()
{
    test("test_array_3 : simple update using utext array");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 503 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 503 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 503 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 1", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 505 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 505 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 505 "utext.pgc"


	count = 0;    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 508 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 508 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 508 "utext.pgc"

	printf ("find %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 0;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 512 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 512 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 512 "utext.pgc"

	printf ("find %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple delete using utext array
void test_array_4()
{
    test("test_array_4 : simple delete using utext array");
    init_var();

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 524 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 524 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 524 "utext.pgc"

    
    count = 100;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 527 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 527 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 527 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 528 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 528 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 528 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 100;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 532 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 532 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 532 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 533 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 533 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 533 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using utext array
void test_array_5()
{
    test("test_array_5 : simple insert using utext array");
    init_var();

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 545 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 545 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 545 "utext.pgc"

    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 547 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 547 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 547 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 548 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 548 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 548 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 551 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 551 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 551 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 552 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 552 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 552 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into utext array
void test_array_6()
{
    test("test_array_6 : prepared select into utext array");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=?");
#line 564 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 564 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 564 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"4",(long)1,(long)1,strlen("4"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 566 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 566 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 566 "utext.pgc"


	print_array();	

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 570 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 570 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 570 "utext.pgc"
 
}

//prepared select using utext array
void test_array_7()
{
    test("test_array_7 : prepared select using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 578 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 578 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 578 "utext.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 581 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 581 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 581 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 582 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 582 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 582 "utext.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 585 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 585 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 585 "utext.pgc"

}

//prepared update using utext array
void test_array_8()
{
    test("test_array_8 : prepared update using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 593 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 593 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 593 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 595 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 595 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 595 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 596 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 596 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 596 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 597 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 597 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 597 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 598 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 598 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 598 "utext.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 601 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 601 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 601 "utext.pgc"

	
	init_table_value();
}

//prepared delete using utext array
void test_array_9()
{
    test("test_array_9 : prepared delete using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 611 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 611 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 611 "utext.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 614 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 614 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 614 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 615 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 615 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 615 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 616 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 616 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 616 "utext.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 619 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 619 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 619 "utext.pgc"

	
	init_table_value();
}

//prepared insert using utext array
void test_array_10()
{
    test("test_array_10 : prepared insert using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 629 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 629 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 629 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, ?)");
#line 631 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 631 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 631 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"13",(long)2,(long)1,strlen("13"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 632 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 632 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 632 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"15",(long)2,(long)1,strlen("15"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 633 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 633 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 633 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 634 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 634 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 634 "utext.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 637 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 637 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 637 "utext.pgc"

	
	init_table_value();
}

//Open cursor using utext array
void test_array_11()
{
    test("test_array_11 : Open cursor using utext array");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 648 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 648 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 648 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 649 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 649 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 649 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 651 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 651 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 651 "utext.pgc"

	/* declare cursor_array_11 cursor for $1 */
#line 652 "utext.pgc"

	{ ECPGopen("cursor_array_11", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_array_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 653 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 653 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 653 "utext.pgc"

	{ ECPGfetch("cursor_array_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 654 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 654 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 654 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGclose("cursor_array_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_array_11", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 656 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 656 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 656 "utext.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 657 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 657 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 657 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 658 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 658 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 658 "utext.pgc"

}

//Fecth cursor into utext array
void test_array_12()
{
    test("test_array_12 : Fecth cursor into utext array");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 667 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 667 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 667 "utext.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=3");
#line 668 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 668 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 668 "utext.pgc"

	/* declare cursor_array_12 cursor for $1 */
#line 669 "utext.pgc"

	{ ECPGopen("cursor_array_12", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_array_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 670 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 670 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 670 "utext.pgc"

	{ ECPGfetch("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 671 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 671 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 671 "utext.pgc"

	{ ECPGfetch("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[1]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 672 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 672 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 672 "utext.pgc"

	{ ECPGfetch("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 673 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 673 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 673 "utext.pgc"

	{ ECPGclose("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_array_12", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 674 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 674 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 674 "utext.pgc"

	
	print_array();
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 678 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 678 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 678 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 679 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 679 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 679 "utext.pgc"

}

//Insert array with L string inited
void test_array_13()
{
    test("test_array_13 : Insert array with L string inited");
    init_var();

/* exec sql begin declare section */
    	

#line 689 "utext.pgc"
 utext utext_array13 [] [ VAR_SIZE ] = { L"太𠮷" , L"𠮷太" , L"太平" } ;
/* exec sql end declare section */
#line 690 "utext.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,(utext_array13[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 691 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 691 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 691 "utext.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 21 )", 
	ECPGt_utext,(utext_array13[1]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 692 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 692 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 692 "utext.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 22 )", 
	ECPGt_utext,(utext_array13[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 693 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 693 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 693 "utext.pgc"

    
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 22 and count >= 20", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 696 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 696 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 696 "utext.pgc"

    print_array();
    init_table_value();
}

//simple select into utext pointer
void test_pvar_1()
{
    test("test_pvar_1 : simple select into utext pointer");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 705 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 705 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 705 "utext.pgc"
 
	print_utext(p_utext_var);
	init_var();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 2", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 709 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 709 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 709 "utext.pgc"
 
	print_utext(p_utext_var);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 713 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 713 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 713 "utext.pgc"
 
	print_utext(p_utext_var);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 4", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 717 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 717 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 717 "utext.pgc"
 
	print_utext(p_utext_var);
	init_var();
    init_table_value();
}


//simple select using utext pointer
void test_pvar_2()
{
    test("test_pvar_2 : simple select using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 730 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 730 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 730 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 731 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 731 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 731 "utext.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 735 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 735 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 735 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 736 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 736 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 736 "utext.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}


//simple update using utext pointer
void test_pvar_3()
{
    test("test_pvar_3 : simple update using utext pointer");
    init_var();
    
    count = 0;
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 748 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 748 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 748 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 2", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 749 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 749 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 749 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 3", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 750 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 750 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 750 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 751 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 751 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 751 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_table_value();
}

//simple delete using utext pointer 
void test_pvar_4()
{
    test("test_pvar_4 : simple delete using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 763 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 763 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 763 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 764 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 764 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 764 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 765 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 765 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 765 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 769 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 769 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 769 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 770 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 770 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 770 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 771 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 771 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 771 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using utext pointer
void test_pvar_5()
{
    test("test_pvar_5 : simple insert using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 783 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 783 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 783 "utext.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 784 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 784 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 784 "utext.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 786 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 786 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 786 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into utext pointer
void test_pvar_6()
{
    test("test_pvar_6 : prepared select into utext pointer");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=?");
#line 798 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 798 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 798 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 800 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 800 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 800 "utext.pgc"

	print_utext(p_utext_var);
	
	init_var();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 804 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 804 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 804 "utext.pgc"

	print_utext(p_utext_var);
    init_table_value();
    
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 808 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 808 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 808 "utext.pgc"
 
    
}

//prepared select using utext pointer
void test_pvar_7()
{
    test("test_pvar_7 : prepared select using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 818 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 818 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 818 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 820 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 820 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 820 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 821 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 821 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 821 "utext.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 824 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 824 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 824 "utext.pgc"

}

//prepared update using utext pointer
void test_pvar_8()
{
    test("test_pvar_8 : prepared update using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 833 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 833 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 833 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 835 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 835 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 835 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 836 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 836 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 836 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 837 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 837 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 837 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 838 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 838 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 838 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 841 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 841 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 841 "utext.pgc"

	
	init_table_value();
}

//prepared delete using utext pointer
void test_pvar_9()
{
    test("test_pvar_9 : prepared delete using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 852 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 852 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 852 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 854 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 854 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 854 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 855 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 855 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 855 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 856 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 856 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 856 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 859 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 859 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 859 "utext.pgc"

	
	init_table_value();
}

//prepared insert using utext pointer
void test_pvar_10()
{
    test("test_pvar_10 : prepared insert using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 870 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 870 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 870 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, 13)");
#line 872 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 872 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 872 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 873 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 873 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 873 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 874 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 874 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 874 "utext.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 877 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 877 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 877 "utext.pgc"

	
	init_table_value();
}

//Open cursor using utext pointer
void test_pvar_11()
{
    test("test_pvar_11 : Open cursor using utext pointer");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 888 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 888 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 888 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 889 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 889 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 889 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 891 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 891 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 891 "utext.pgc"

	/* declare cursor_pvar_11 cursor for $1 */
#line 892 "utext.pgc"

	{ ECPGopen("cursor_pvar_11", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_pvar_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 893 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 893 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 893 "utext.pgc"

	{ ECPGfetch("cursor_pvar_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_pvar_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 894 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 894 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 894 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGclose("cursor_pvar_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_pvar_11", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 896 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 896 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 896 "utext.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 897 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 897 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 897 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 898 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 898 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 898 "utext.pgc"

}

//Fecth cursor into utext pointer
void test_pvar_12()
{
    test("test_pvar_12 : Fecth cursor into utext pointer");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 907 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 907 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 907 "utext.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=1");
#line 908 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 908 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 908 "utext.pgc"

	/* declare cursor_pvar_12 cursor for $1 */
#line 909 "utext.pgc"

	{ ECPGopen("cursor_pvar_12", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_pvar_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 910 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 910 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 910 "utext.pgc"

	{ ECPGfetch("cursor_pvar_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_pvar_12", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 911 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 911 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 911 "utext.pgc"

	{ ECPGclose("cursor_pvar_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_pvar_12", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 912 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 912 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 912 "utext.pgc"

	
	print_utext(p_utext_var);
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 916 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 916 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 916 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 917 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 917 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 917 "utext.pgc"

}

//simple insert utext pointer with L string inited
void test_pvar_13()
{
/* exec sql begin declare section */
		

#line 924 "utext.pgc"
 utext * utext_local_pvar = L"太𠮷" ;
/* exec sql end declare section */
#line 925 "utext.pgc"

    test("test_pvar_13 : simple insert utext pointer with L string inited");
	init_var();
	
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 16 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 929 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 929 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 929 "utext.pgc"

    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 16", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 931 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 931 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 931 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
    init_table_value();
}

//Buck insert utext array into table
void test_buckinsert_1()
{
    test("test_buckinsert_1 : Buck insert utext array into table");
    init_var();
    
 	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 944 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 944 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 944 "utext.pgc"
 

    // truncate table
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate tb1", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 947 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 947 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 947 "utext.pgc"

	
	//buck insert 
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "BULK_INSERT_V01_I0058R0000 insert into tb1 (Item , count) values ( $1  , $2  )", 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(count_array),(long)1,(long)4,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOLT);
#line 950 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 950 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 950 "utext.pgc"

	
    //check the results
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1", ECPGt_EOIT, 
	ECPGt_int,&(total_tuples),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 953 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 953 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 953 "utext.pgc"

    printf ("Total tuples in tb1 = %d\n", total_tuples);
    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 956 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 956 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 956 "utext.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 959 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 959 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 959 "utext.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);	
	
	init_var();
    init_table_value();	
}

void test_all()
{
	test_var_1();
	test_var_2();
	test_var_3();
	test_var_4();
	test_var_5();
	test_var_6();
	test_var_7();
	test_var_8();
	test_var_9();
	test_var_10();
	test_var_11();
	test_var_12();
    test_var_13();
	test_array_1();
	test_array_2();
	test_array_3();
	test_array_4();
	test_array_5();
	test_array_6();
	test_array_7();
	test_array_8();
	test_array_9();
	test_array_10();
	test_array_11();
	test_array_12();
    test_array_13();
	test_pvar_1();
	test_pvar_2();
	test_pvar_3();
	test_pvar_4();
	test_pvar_5();
	test_pvar_6();
	test_pvar_7();
	test_pvar_8();
	test_pvar_9();
	test_pvar_10();
	test_pvar_11();
	test_pvar_12();
    test_pvar_13();
	test_buckinsert_1();
}

int main(int argc, char *argv[])
{
	ECPGdebug(1, stderr);
    if(test_init() !=0)
        return -1;
  
	test_all();
	//test_var_13();
    //test_array_13();
    //test_pvar_13();
	
    test_finish();

	return 0;
}