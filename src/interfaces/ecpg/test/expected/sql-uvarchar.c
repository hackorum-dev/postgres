/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#define ECPG_ENABLE_UTEXT 1
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "uvarchar.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "uvarchar.pgc"


#define test(msg) printf("\n%s\n",msg)
#define VAR_SIZE  20
#define ARRAY_SIZE 4

/* Following is UTF8 and UTF16 characters mapping table */
/*太𠮷𠜱平洋𠱓大西洋印度洋北冰洋
0x592A,0x5E73,0x6D0B,0x5927,0x897F,0x6D0B,0x5370,0x5EA6,
0x6D0B,0x5317,0x51B0,0x6D0B,0x548C,0x5357,0x51B0,0x6D0B,
0x0000,0x0000,0x0000,0x0000*/

/*足球篮球羽毛球乒乓球橄榄球棒球冰球
0x8DB3,0x7403,0x7BEE,0x7403,0x7FBD,0x6BDB,0x7403,0x4E52,
0x4E53,0x7403,0x6A44,0x6984,0x7403,0x68D2,0x7403,0x51B0,
0x7403,0x0000,0x0000,0x0000
*/

/*世界杯每隔四年就会举行一次每次𠲖个球队
0x4E16,0x754C,0x676F,0x6BCF,0x9694,0x56DB,0x5E74,0x5C31,
0x4F1A,0x4E3E,0x884C,0x4E00,0x6B21,0x6BCF,0x6B21,0x0033,
0x0032,0x4E2A,0x7403,0x961F
*/

/* 亚洲欧洲非洲大洋洲北美洲南美洲南极洲没有北极洲
0x4E9A,0x6D32,0x6B27,0x6D32,0x975E,0x6D32,0x5927,0x6D0B,
0x6D32,0x5317,0x7F8E,0x6D32,0x5357,0x7F8E,0x6D32,0x5357,
0x6781,0x6D32,0x6CA1,0x6709,0x5317,0x6781,0x6D32
*/

/* exec sql begin declare section */
	    
	    
	
			  
	
			
	     
	       

#line 36 "uvarchar.pgc"
  struct uvarchar_1  { int len; utext arr[ VAR_SIZE ]; }  uvarchar_var ;
 
#line 37 "uvarchar.pgc"
  struct uvarchar_2  { int len; utext arr[ VAR_SIZE ]; }  uvarchar_array [ ARRAY_SIZE ] ;
 
#line 39 "uvarchar.pgc"
 int uvarchar_var_ind ;
 
#line 41 "uvarchar.pgc"
 int count ;
 
#line 42 "uvarchar.pgc"
 int count_array [ 4 ] = { 1 , 2 , 3 , 4 } ;
 
#line 43 "uvarchar.pgc"
 int total_tuples = 0 ;
/* exec sql end declare section */
#line 44 "uvarchar.pgc"


void print_uvarchar(void);
void print_uvarchar_ind(int uvarchar_var_ind);
void print_local_uvarchar(utext *utext_var, int var_len);
void print_array(void);
int test_init(void);
void test_finish(void);
void init_table_value(void);
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
void test_var_14(void);
void test_var_15(void);
void test_var_16(void);
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
void test_array_14(void);
void test_array_15(void);
void test_array_16(void);

void test_all(void);

void print_uvarchar()
{
    int i;

    printf ("---->uvarchar variable,len=%d:\n",uvarchar_var.len);
	for(i=0; i<VAR_SIZE; i++)
	{
	    printf ("0x%04X  ", uvarchar_var.arr[i]);
	    if(i>6 && (i+1)%8==0)
	        printf("\n");
	}
	printf("\n");
}

void print_uvarchar_ind(int uvarchar_var_ind)
{
	printf("uvarchar_var_ind = %d\n",uvarchar_var_ind);
}

void print_local_uvarchar(utext *utext_var, int var_len)
{
    int i;

    printf ("---->uvarchar variable,len=%d:\n",var_len);
	for(i=0; i<20; i++)
	{
	    printf ("0x%04X  ", utext_var[i]);
	    if(i>6 && (i+1)%8==0)
	        printf("\n");
	}
	printf("\n");
}

void print_array()
{
    int i,j;

	for(i=0; i<ARRAY_SIZE; i++)
	{
        printf ("---->uvarchar array[%d]:(len=%d)\n", i,uvarchar_array[i].len);
        
	    for(j=0; j<VAR_SIZE; j++)
	    {
	        printf ("0x%04X  ", uvarchar_array[i].arr[j]);
	        if(j>6 && (j+1)%8==0)
	            printf("\n");
	    }
	    printf("\n");
	}

	printf("\n");
}

int test_init()
{
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); }
#line 145 "uvarchar.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);}
#line 147 "uvarchar.pgc"

	/* exec sql whenever sql_warning  sqlprint ; */
#line 148 "uvarchar.pgc"

	/* exec sql whenever sqlerror  sqlprint ; */
#line 149 "uvarchar.pgc"


	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "set client_encoding = 'UTF8'", ECPGt_EOIT, ECPGt_EORT);
#line 151 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 151 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 151 "uvarchar.pgc"

	
    //initialization of test table
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "create table tb1 ( Item varchar , count integer )", ECPGt_EOIT, ECPGt_EORT);
#line 154 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 154 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 154 "uvarchar.pgc"

	
	init_table_value();
	
	return 0;
}

void test_finish()
{
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "drop table tb1", ECPGt_EOIT, ECPGt_EORT);
#line 163 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 163 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 163 "uvarchar.pgc"

	{ ECPGdisconnect(__LINE__, "ALL");
#line 164 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 164 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 164 "uvarchar.pgc"

}


void init_table_value()
{
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "truncate tb1", ECPGt_EOIT, ECPGt_EORT);
#line 170 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 170 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 170 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋' , 1 )", ECPGt_EOIT, ECPGt_EORT);
#line 172 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 172 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 172 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '足球篮球羽毛球乒乓球橄榄球棒球冰球' , 2 )", ECPGt_EOIT, ECPGt_EORT);
#line 173 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 173 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 173 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '世界杯每隔四年就会举行一次每次𠲖个球队' , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 174 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 174 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 174 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '亚洲欧洲非洲大洋洲北美洲南美洲南极洲没有北极洲' , 4 )", ECPGt_EOIT, ECPGt_EORT);
#line 175 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 175 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 175 "uvarchar.pgc"

    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 ( count ) values ( 8 )", ECPGt_EOIT, ECPGt_EORT);
#line 177 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 177 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 177 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 ( count ) values ( 9 )", ECPGt_EOIT, ECPGt_EORT);
#line 178 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 178 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 178 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 ( count ) values ( 10 )", ECPGt_EOIT, ECPGt_EORT);
#line 179 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 179 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 179 "uvarchar.pgc"

}

void init_var()
{
    int i;
    memset((void*)&uvarchar_var,'a',sizeof(uvarchar_var));
    uvarchar_var.len = 0;
    
    memset((char*)uvarchar_array,'a',sizeof(uvarchar_array)*ARRAY_SIZE);
    
    for(i=0;i<ARRAY_SIZE;i++)
    {
        uvarchar_array[i].len = 0;
    }

    uvarchar_var_ind = 0;
}

//simple select into uvarchar
void test_var_1()
{
    test("test_var_1 : simple select into uvarchar var");
    init_var();
    //print_uvarchar();
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 205 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 205 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 205 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
	

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 2", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 211 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 211 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 211 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 216 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 216 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 216 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 4", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 221 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 221 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 221 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
}


//simple select using uvarchar
void test_var_2()
{
    test("test_var_2 : simple select using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 234 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 234 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 234 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 235 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 235 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 235 "uvarchar.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 238 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 238 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 238 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 239 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 239 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 239 "uvarchar.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using uvarchar
void test_var_3()
{
    test("test_var_3 : simple update using uvarchar");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 249 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 249 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 249 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 2", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 250 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 250 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 250 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 3", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 251 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 251 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 251 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 252 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 252 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 252 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_table_value();
}

//simple delete using uvarchar var
void test_var_4()
{
    test("test_var_4 : simple delete using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 264 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 264 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 264 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 265 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 265 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 265 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 266 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 266 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 266 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 269 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 269 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 269 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 270 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 270 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 270 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 271 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 271 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 271 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using uvarchar var
void test_var_5()
{
    test("test_var_5 : simple insert using uvarchar");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 283 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 283 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 283 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 284 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 284 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 284 "uvarchar.pgc"


    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 287 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 287 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 287 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 288 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 288 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 288 "uvarchar.pgc"



	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 291 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 291 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 291 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 294 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 294 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 294 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into uvarchar var
void test_var_6()
{
    test("test_var_6 : prepared select into uvarchar var");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=?");
#line 306 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 306 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 306 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 308 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 308 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 308 "uvarchar.pgc"

	print_uvarchar();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 311 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 311 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 311 "uvarchar.pgc"

	print_uvarchar();

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 314 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 314 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 314 "uvarchar.pgc"
 
}

//prepared select using uvarchar var
void test_var_7()
{
    test("test_var_7 : prepared select using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 323 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 323 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 323 "uvarchar.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 325 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 325 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 325 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 326 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 326 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 326 "uvarchar.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 329 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 329 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 329 "uvarchar.pgc"

}

//prepared update using uvarchar var
void test_var_8()
{
    test("test_var_8 : prepared update using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 338 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 338 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 338 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 340 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 340 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 340 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 341 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 341 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 341 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 342 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 342 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 342 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 343 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 343 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 343 "uvarchar.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 346 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 346 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 346 "uvarchar.pgc"

	
	init_table_value();
}

//prepared delete using uvarchar var
void test_var_9()
{
    test("test_var_9 : prepared delete using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 357 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 357 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 357 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 359 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 359 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 359 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 360 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 360 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 360 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 361 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 361 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 361 "uvarchar.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 364 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 364 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 364 "uvarchar.pgc"

	
	init_table_value();
}

//prepared insert using uvarchar var
void test_var_10()
{
    test("test_var_10 : prepared insert using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 375 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 375 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 375 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, 13)");
#line 377 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 377 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 377 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 378 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 378 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 378 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 379 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 379 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 379 "uvarchar.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 382 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 382 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 382 "uvarchar.pgc"

	
	init_table_value();
}

//Open cursor using uvarchar var
void test_var_11()
{
    test("test_var_11 : Open cursor using uvarchar var");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 393 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 393 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 393 "uvarchar.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 394 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 394 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 394 "uvarchar.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 396 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 396 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 396 "uvarchar.pgc"

	/* declare cursor_var_11 cursor for $1 */
#line 397 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_var_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 398 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 398 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 398 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_var_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 399 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 399 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 399 "uvarchar.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_var_11", ECPGt_EOIT, ECPGt_EORT);
#line 401 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 401 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 401 "uvarchar.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 402 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 402 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 402 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 403 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 403 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 403 "uvarchar.pgc"

}

//Fecth cursor into uvarchar var
void test_var_12()
{
    test("test_var_12 : Fecth cursor into uvarchar var");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 412 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 412 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 412 "uvarchar.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=1");
#line 413 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 413 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 413 "uvarchar.pgc"

	/* declare cursor_var_12 cursor for $1 */
#line 414 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_var_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 415 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 415 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 415 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_var_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 416 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 416 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 416 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_var_12", ECPGt_EOIT, ECPGt_EORT);
#line 417 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 417 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 417 "uvarchar.pgc"

	
	print_uvarchar();
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 421 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 421 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 421 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 422 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 422 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 422 "uvarchar.pgc"

}

//simple insert using uvarchar with L string inited
void test_var_13()
{
/* exec sql begin declare section */
		

#line 429 "uvarchar.pgc"
  struct uvarchar_3  { int len; utext arr[ VAR_SIZE ]; }  uvarchar_local_var ;
/* exec sql end declare section */
#line 430 "uvarchar.pgc"


    test("test_var_13 : simple insert using uvarchar with L string inited");
    init_var();
    
    memset((char*)&uvarchar_local_var,0,sizeof(uvarchar_local_var));
    
    memcpy((char*)uvarchar_local_var.arr, L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋", sizeof(L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋"));
    uvarchar_local_var.len = sizeof(L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋")/4;
    
    printf("uvarchar_local_var.len = %d\n",uvarchar_local_var.len);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 16 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 442 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 442 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 442 "uvarchar.pgc"


	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 444 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 444 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 444 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 16", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 447 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 447 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 447 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
    
	init_table_value();
}

//Open cursor using utext var directly in WHERE Clause
void test_var_14()
{
/* exec sql begin declare section */
		

#line 458 "uvarchar.pgc"
  struct uvarchar_4  { int len; utext arr[ 20 + 1 ]; }  uvarchar_local_var ;
/* exec sql end declare section */
#line 459 "uvarchar.pgc"

    memset((char*)&uvarchar_local_var,0x00,sizeof(uvarchar_local_var));
    
    test("test_var_14 : Open cursor using utext var directly in WHERE Clause");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 465 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 465 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 465 "uvarchar.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_4), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 466 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 466 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 466 "uvarchar.pgc"

	ECPGset_var( 0, &( uvarchar_local_var ), __LINE__);\
 /* declare cursor_var_14 cursor for select count from tb1 where Item = $1  */
#line 467 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_var_14 cursor for select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_4), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 468 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 468 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 468 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_var_14", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 469 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 469 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 469 "uvarchar.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_var_14", ECPGt_EOIT, ECPGt_EORT);
#line 472 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 472 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 472 "uvarchar.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 474 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 474 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 474 "uvarchar.pgc"

}

//Test uvarchar_var working with NULL without indicator
void test_var_15()
{
/* exec sql begin declare section */
    	

#line 481 "uvarchar.pgc"
  struct uvarchar_5  { int len; utext arr[ 20 + 1 ]; }  uvarchar_local_var ;
/* exec sql end declare section */
#line 482 "uvarchar.pgc"

    test("test_var_15 : Test uvarchar_var working with NULL without indicator");

    memset((char*)&uvarchar_local_var,'a',sizeof(uvarchar_local_var));
    uvarchar_local_var.len=10;
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_5), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 487 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 487 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 487 "uvarchar.pgc"

    print_local_uvarchar(uvarchar_local_var.arr,20);
    
    
    memset((char*)&uvarchar_local_var,0x00,sizeof(uvarchar_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_5), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 492 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 492 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 492 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_5), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 493 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 493 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 493 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_5), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 494 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 494 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 494 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 495 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 495 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 495 "uvarchar.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//uvarchar_var working with NULL with indicator
void test_var_16()
{
/* exec sql begin declare section */
    	
         

#line 505 "uvarchar.pgc"
  struct uvarchar_6  { int len; utext arr[ 20 + 1 ]; }  uvarchar_local_var ;
 
#line 506 "uvarchar.pgc"
 int uvarchar_var_ind = - 1 ;
/* exec sql end declare section */
#line 507 "uvarchar.pgc"

    test("test_var_16 : Test uvarchar_var working with NULL with indicator");

    memset((char*)&uvarchar_local_var,'a',sizeof(uvarchar_local_var));
    uvarchar_local_var.len=0;
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_6), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 512 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 512 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 512 "uvarchar.pgc"

    print_local_uvarchar(uvarchar_local_var.arr,20);
    print_uvarchar_ind(uvarchar_var_ind);
    
    memset((char*)&uvarchar_local_var,0x00,sizeof(uvarchar_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_6), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 517 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 517 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 517 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_6), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 518 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 518 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 518 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)20 + 1,(long)1,sizeof(struct uvarchar_6), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 519 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 519 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 519 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 520 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 520 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 520 "uvarchar.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}


//simple select into uvarchar array
void test_array_1()
{
    test("test_array_1 : simple select into uvarchar array");
    init_var();
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 533 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 533 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 533 "uvarchar.pgc"


	print_array();
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 538 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 538 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 538 "uvarchar.pgc"
 
	print_array();
	init_var();
}


//simple select using uvarchar array
void test_array_2()
{
    test("test_array_2 : simple select using uvarchar array");
	init_var();
	    
 	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 550 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 550 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 550 "uvarchar.pgc"
 

	count = 0;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 553 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 553 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 553 "uvarchar.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	count = 0;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 557 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 557 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 557 "uvarchar.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using uvarchar array
void test_array_3()
{
    test("test_array_3 : simple update using uvarchar array");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 567 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 567 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 567 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 1", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 569 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 569 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 569 "uvarchar.pgc"


	count = 0;    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 572 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 572 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 572 "uvarchar.pgc"

	printf ("find %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 0;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 576 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 576 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 576 "uvarchar.pgc"

	printf ("find %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple delete using uvarchar array
void test_array_4()
{
    test("test_array_4 : simple delete using uvarchar array");
    init_var();

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 588 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 588 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 588 "uvarchar.pgc"

    
    count = 100;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 591 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 591 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 591 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 592 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 592 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 592 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 100;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 596 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 596 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 596 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 597 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 597 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 597 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using uvarchar array
void test_array_5()
{
    test("test_array_5 : simple insert using uvarchar array");
    init_var();

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 609 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 609 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 609 "uvarchar.pgc"

    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 611 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 611 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 611 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 612 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 612 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 612 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 615 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 615 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 615 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 616 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 616 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 616 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into uvarchar array
void test_array_6()
{
    test("test_array_6 : prepared select into uvarchar array");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=?");
#line 628 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 628 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 628 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 630 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 630 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 630 "uvarchar.pgc"


	print_array();	

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 634 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 634 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 634 "uvarchar.pgc"
 
}

//prepared select using uvarchar array
void test_array_7()
{
    test("test_array_7 : prepared select using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 642 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 642 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 642 "uvarchar.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 645 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 645 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 645 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 646 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 646 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 646 "uvarchar.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 649 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 649 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 649 "uvarchar.pgc"

}

//prepared update using uvarchar array
void test_array_8()
{
    test("test_array_8 : prepared update using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 657 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 657 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 657 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 659 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 659 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 659 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 660 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 660 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 660 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 661 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 661 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 661 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 662 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 662 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 662 "uvarchar.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 665 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 665 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 665 "uvarchar.pgc"

	
	init_table_value();
}

//prepared delete using uvarchar array
void test_array_9()
{
    test("test_array_9 : prepared delete using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 675 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 675 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 675 "uvarchar.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 678 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 678 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 678 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 679 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 679 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 679 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 680 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 680 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 680 "uvarchar.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 683 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 683 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 683 "uvarchar.pgc"

	
	init_table_value();
}

//prepared insert using uvarchar array
void test_array_10()
{
    test("test_array_10 : prepared insert using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 693 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 693 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 693 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, ?)");
#line 695 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 695 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 695 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"13",(long)2,(long)1,strlen("13"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 696 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 696 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 696 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"15",(long)2,(long)1,strlen("15"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 697 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 697 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 697 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 698 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 698 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 698 "uvarchar.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 701 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 701 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 701 "uvarchar.pgc"

	
	init_table_value();
}

//Open cursor using uvarchar array
void test_array_11()
{
    test("test_array_11 : Open cursor using uvarchar array");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 712 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 712 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 712 "uvarchar.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 713 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 713 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 713 "uvarchar.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 715 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 715 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 715 "uvarchar.pgc"

	/* declare cursor_array_11 cursor for $1 */
#line 716 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_array_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 717 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 717 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 717 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 718 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 718 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 718 "uvarchar.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_array_11", ECPGt_EOIT, ECPGt_EORT);
#line 720 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 720 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 720 "uvarchar.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 721 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 721 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 721 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 722 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 722 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 722 "uvarchar.pgc"

}

//Fecth cursor into uvarchar array
void test_array_12()
{
    test("test_array_12 : Fecth cursor into uvarchar array");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 731 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 731 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 731 "uvarchar.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=3");
#line 732 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 732 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 732 "uvarchar.pgc"

	/* declare cursor_array_12 cursor for $1 */
#line 733 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_array_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 734 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 734 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 734 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 735 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 735 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 735 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[1]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 736 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 736 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 736 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 737 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 737 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 737 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_array_12", ECPGt_EOIT, ECPGt_EORT);
#line 738 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 738 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 738 "uvarchar.pgc"

	
	print_array();
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 742 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 742 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 742 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 743 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 743 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 743 "uvarchar.pgc"

}

//Insert array with L string inited
void test_array_13()
{
/* exec sql begin declare section */
    	

#line 750 "uvarchar.pgc"
  struct uvarchar_7  { int len; utext arr[ VAR_SIZE ]; }  uvarchar_array13 [ 3 ] ;
/* exec sql end declare section */
#line 751 "uvarchar.pgc"

    test("test_array_13 : Insert array with L string inited");
    
    memset((char*)&uvarchar_array13,0,sizeof(uvarchar_array13));
    
    memcpy((char*)uvarchar_array13[0].arr, L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋", sizeof(L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋"));
    uvarchar_array13[0].len = sizeof(L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋")/4;
    memcpy((char*)uvarchar_array13[1].arr, L"足球篮球羽毛球乒乓球橄榄球棒球冰球", sizeof(L"足球篮球羽毛球乒乓球橄榄球棒球冰球"));
    uvarchar_array13[1].len = sizeof(L"足球篮球羽毛球乒乓球橄榄球棒球冰球")/4;
    memcpy((char*)uvarchar_array13[2].arr, L"世界杯每隔四年就会举行一次每次𠲖个球队", sizeof(L"世界杯每隔四年就会举行一次每次𠲖个球队"));
    uvarchar_array13[2].len = sizeof(L"世界杯每隔四年就会举行一次每次𠲖个球队")/4;
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_uvarchar,&(uvarchar_array13[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_7), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 763 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 763 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 763 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 21 )", 
	ECPGt_uvarchar,&(uvarchar_array13[1]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_7), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 764 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 764 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 764 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 22 )", 
	ECPGt_uvarchar,&(uvarchar_array13[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_7), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 765 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 765 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 765 "uvarchar.pgc"

    
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 22 and count >= 20", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 768 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 768 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 768 "uvarchar.pgc"

    print_array();
    init_table_value();

}

//uvarchar array working with NULL without using indicator
void test_array_15()
{
/* exec sql begin declare section */
    	

#line 778 "uvarchar.pgc"
  struct uvarchar_8  { int len; utext arr[ 20 ]; }  uvarchar_local_array [ 3 ] ;
/* exec sql end declare section */
#line 779 "uvarchar.pgc"

    test("test_array_15 : Test uvarchar array working with NULL without using indicator");
    memset(uvarchar_local_array,'a',sizeof(uvarchar_local_array));
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count >= 8 and count <= 10", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_local_array),(long)20,(long)3,sizeof(struct uvarchar_8), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 783 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 783 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 783 "uvarchar.pgc"

	
	print_local_uvarchar(uvarchar_local_array[0].arr, 20);
	print_local_uvarchar(uvarchar_local_array[1].arr, 20);
	print_local_uvarchar(uvarchar_local_array[2].arr, 20);

    
    memset(uvarchar_local_array,0x00,sizeof(uvarchar_local_array));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_uvarchar,&(uvarchar_local_array[0]),(long)20,(long)1,sizeof(struct uvarchar_8), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 791 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 791 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 791 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_uvarchar,&(uvarchar_local_array[1]),(long)20,(long)1,sizeof(struct uvarchar_8), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 792 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 792 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 792 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_uvarchar,&(uvarchar_local_array[2]),(long)20,(long)1,sizeof(struct uvarchar_8), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 793 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 793 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 793 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 794 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 794 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 794 "uvarchar.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//uvarchar array working with NULL using indicator
void test_array_16()
{
/* exec sql begin declare section */
    	
         

#line 804 "uvarchar.pgc"
  struct uvarchar_9  { int len; utext arr[ 20 ]; }  uvarchar_local_array [ 3 ] ;
 
#line 805 "uvarchar.pgc"
 int uvarchar_array_ind [ 3 ] ;
/* exec sql end declare section */
#line 806 "uvarchar.pgc"

    test("test_array_16 : Test uvarchar array working with NULL using indicator");
    memset(uvarchar_local_array,'a',sizeof(uvarchar_local_array));
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count >= 8 and count <= 10", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_local_array),(long)20,(long)3,sizeof(struct uvarchar_9), 
	ECPGt_int,(uvarchar_array_ind),(long)1,(long)3,sizeof(int), ECPGt_EORT);
#line 810 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 810 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 810 "uvarchar.pgc"

	
	
	print_local_uvarchar(uvarchar_local_array[0].arr, 20);
	print_local_uvarchar(uvarchar_local_array[1].arr, 20);
	print_local_uvarchar(uvarchar_local_array[2].arr, 20);
	
	print_uvarchar_ind(uvarchar_array_ind[0]);
	print_uvarchar_ind(uvarchar_array_ind[1]);
	print_uvarchar_ind(uvarchar_array_ind[2]);
	    
    memset(uvarchar_local_array,0x00,sizeof(uvarchar_local_array));
    uvarchar_array_ind[0]=-1;
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_uvarchar,&(uvarchar_local_array[0]),(long)20,(long)1,sizeof(struct uvarchar_9), 
	ECPGt_int,&(uvarchar_array_ind[0]),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 823 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 823 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 823 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_uvarchar,&(uvarchar_local_array[1]),(long)20,(long)1,sizeof(struct uvarchar_9), 
	ECPGt_int,&(uvarchar_array_ind[0]),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 824 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 824 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 824 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_uvarchar,&(uvarchar_local_array[2]),(long)20,(long)1,sizeof(struct uvarchar_9), 
	ECPGt_int,&(uvarchar_array_ind[0]),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 825 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 825 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 825 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 826 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 826 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 826 "uvarchar.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
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
    test_var_14();
    test_var_15();
    test_var_16();
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
    test_array_15();
    test_array_16();
}

int main() 
{
//	ECPGdebug(1, stderr);
    if(test_init() !=0)
        return -1;
    
    test_all();
    test_finish();

	return 0;
}
