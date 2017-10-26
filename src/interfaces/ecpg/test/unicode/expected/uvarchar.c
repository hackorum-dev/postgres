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


#line 1 "./../regression.h"






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
void test_buckinsert_1(void);

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
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 125 "uvarchar.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);}
#line 127 "uvarchar.pgc"

	/* exec sql whenever sql_warning  sqlprint ; */
#line 128 "uvarchar.pgc"

	/* exec sql whenever sqlerror  sqlprint ; */
#line 129 "uvarchar.pgc"


    //initialization of test table
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table tb1 ( Item varchar , count integer )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 132 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 132 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 132 "uvarchar.pgc"

	
	init_table_value();
	
	return 0;
}

void test_finish()
{
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table tb1", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 141 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 141 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 141 "uvarchar.pgc"

	{ ECPGdisconnect(__LINE__, "ALL");
#line 142 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 142 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 142 "uvarchar.pgc"

}


void init_table_value()
{
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate tb1", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 148 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 148 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 148 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋' , 1 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 150 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 150 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 150 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '足球篮球羽毛球乒乓球橄榄球棒球冰球' , 2 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 151 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 151 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 151 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '世界杯每隔四年就会举行一次每次𠲖个球队' , 3 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 152 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 152 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 152 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( '亚洲欧洲非洲大洋洲北美洲南美洲南极洲没有北极洲' , 4 )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 153 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 153 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 153 "uvarchar.pgc"

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
    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 179 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 179 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 179 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
	

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 2", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 185 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 185 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 185 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 190 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 190 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 190 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 4", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 195 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 195 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 195 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
	init_var();
}


//simple select using uvarchar
void test_var_2()
{
    test("test_var_2 : simple select using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 208 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 208 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 208 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 209 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 209 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 209 "uvarchar.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 212 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 212 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 212 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 213 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 213 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 213 "uvarchar.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using uvarchar
void test_var_3()
{
    test("test_var_3 : simple update using uvarchar");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 223 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 223 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 223 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 2", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 224 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 224 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 224 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 3", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 225 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 225 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 225 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 226 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 226 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 226 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_table_value();
}

//simple delete using uvarchar var
void test_var_4()
{
    test("test_var_4 : simple delete using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 238 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 238 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 238 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 239 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 239 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 239 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 240 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 240 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 240 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 243 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 243 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 243 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 244 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 244 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 244 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 245 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 245 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 245 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using uvarchar var
void test_var_5()
{
    test("test_var_5 : simple insert using uvarchar");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 257 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 257 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 257 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 258 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 258 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 258 "uvarchar.pgc"


    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 261 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 261 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 261 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 262 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 262 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 262 "uvarchar.pgc"



	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 265 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 265 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 265 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 268 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 268 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 268 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into uvarchar var
void test_var_6()
{
    test("test_var_6 : prepared select into uvarchar var");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=?");
#line 280 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 280 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 280 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 282 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 282 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 282 "uvarchar.pgc"

	print_uvarchar();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 285 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 285 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 285 "uvarchar.pgc"

	print_uvarchar();

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 288 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 288 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 288 "uvarchar.pgc"
 
}

//prepared select using uvarchar var
void test_var_7()
{
    test("test_var_7 : prepared select using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 297 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 297 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 297 "uvarchar.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 299 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 299 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 299 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 300 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 300 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 300 "uvarchar.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 303 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 303 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 303 "uvarchar.pgc"

}

//prepared update using uvarchar var
void test_var_8()
{
    test("test_var_8 : prepared update using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 312 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 312 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 312 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 314 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 314 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 314 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 315 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 315 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 315 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 316 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 316 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 316 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 317 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 317 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 317 "uvarchar.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 320 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 320 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 320 "uvarchar.pgc"

	
	init_table_value();
}

//prepared delete using uvarchar var
void test_var_9()
{
    test("test_var_9 : prepared delete using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 331 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 331 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 331 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 333 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 333 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 333 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 334 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 334 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 334 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 335 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 335 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 335 "uvarchar.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 338 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 338 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 338 "uvarchar.pgc"

	
	init_table_value();
}

//prepared insert using uvarchar var
void test_var_10()
{
    test("test_var_10 : prepared insert using uvarchar var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 349 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 349 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 349 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, 13)");
#line 351 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 351 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 351 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 352 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 352 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 352 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 353 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 353 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 353 "uvarchar.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 356 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 356 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 356 "uvarchar.pgc"

	
	init_table_value();
}

//Open cursor using uvarchar var
void test_var_11()
{
    test("test_var_11 : Open cursor using uvarchar var");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 367 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 367 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 367 "uvarchar.pgc"
   
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 368 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 368 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 368 "uvarchar.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 370 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 370 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 370 "uvarchar.pgc"

	/* declare cursor_var_11 cursor for $1 */
#line 371 "uvarchar.pgc"

	{ ECPGopen("cursor_var_11", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_var_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 372 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 372 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 372 "uvarchar.pgc"

	{ ECPGfetch("cursor_var_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_var_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 373 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 373 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 373 "uvarchar.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGclose("cursor_var_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_var_11", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 375 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 375 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 375 "uvarchar.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 376 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 376 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 376 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 377 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 377 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 377 "uvarchar.pgc"

}

//Fecth cursor into uvarchar var
void test_var_12()
{
    test("test_var_12 : Fecth cursor into uvarchar var");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 386 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 386 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 386 "uvarchar.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=1");
#line 387 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 387 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 387 "uvarchar.pgc"

	/* declare cursor_var_12 cursor for $1 */
#line 388 "uvarchar.pgc"

	{ ECPGopen("cursor_var_12", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_var_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 389 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 389 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 389 "uvarchar.pgc"

	{ ECPGfetch("cursor_var_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_var_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 390 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 390 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 390 "uvarchar.pgc"

	{ ECPGclose("cursor_var_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_var_12", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 391 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 391 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 391 "uvarchar.pgc"

	
	print_uvarchar();
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 395 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 395 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 395 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 396 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 396 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 396 "uvarchar.pgc"

}

//simple insert using uvarchar with L string inited
void test_var_13()
{
/* exec sql begin declare section */
		

#line 403 "uvarchar.pgc"
  struct uvarchar_3  { int len; utext arr[ VAR_SIZE ]; }  uvarchar_local_var ;
/* exec sql end declare section */
#line 404 "uvarchar.pgc"


    test("test_var_13 : simple insert using uvarchar with L string inited");
    init_var();
    
    memset((char*)&uvarchar_local_var,0,sizeof(uvarchar_local_var));
    
    memcpy((char*)uvarchar_local_var.arr, L"太𠮷𠜱", sizeof(L"太𠮷𠜱"));
    uvarchar_local_var.len = sizeof(L"太𠮷𠜱")/2;
    
    printf("uvarchar_local_var.len = %d\n",uvarchar_local_var.len);
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 16 )", 
	ECPGt_uvarchar,&(uvarchar_local_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 416 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 416 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 416 "uvarchar.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 418 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 418 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 418 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱'\n", count);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 16", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_var),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_int,&(uvarchar_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT, ECPGt_EOLT);
#line 421 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 421 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 421 "uvarchar.pgc"
 
	print_uvarchar();
	print_uvarchar_ind(uvarchar_var_ind);
    
	init_table_value();
}

//simple select into uvarchar array
void test_array_1()
{
    test("test_array_1 : simple select into uvarchar array");
    init_var();
    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 434 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 434 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 434 "uvarchar.pgc"


	print_array();
	init_var();
	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 439 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 439 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 439 "uvarchar.pgc"
 
	print_array();
	init_var();
}


//simple select using uvarchar array
void test_array_2()
{
    test("test_array_2 : simple select using uvarchar array");
	init_var();
	    
 	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 451 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 451 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 451 "uvarchar.pgc"
 

	count = 0;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 454 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 454 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 454 "uvarchar.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	count = 0;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 458 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 458 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 458 "uvarchar.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using uvarchar array
void test_array_3()
{
    test("test_array_3 : simple update using uvarchar array");
    init_var();
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 468 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 468 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 468 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 1", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 470 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 470 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 470 "uvarchar.pgc"


	count = 0;    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 473 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 473 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 473 "uvarchar.pgc"

	printf ("find %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 0;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 477 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 477 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 477 "uvarchar.pgc"

	printf ("find %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple delete using uvarchar array
void test_array_4()
{
    test("test_array_4 : simple delete using uvarchar array");
    init_var();

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 489 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 489 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 489 "uvarchar.pgc"

    
    count = 100;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 492 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 492 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 492 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 493 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 493 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 493 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 100;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 497 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 497 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 497 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 498 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 498 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 498 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using uvarchar array
void test_array_5()
{
    test("test_array_5 : simple insert using uvarchar array");
    init_var();

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 510 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 510 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 510 "uvarchar.pgc"

    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 512 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 512 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 512 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 513 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 513 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 513 "uvarchar.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 516 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 516 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 516 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 517 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 517 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 517 "uvarchar.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into uvarchar array
void test_array_6()
{
    test("test_array_6 : prepared select into uvarchar array");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=?");
#line 529 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 529 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 529 "uvarchar.pgc"

	
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 531 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 531 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 531 "uvarchar.pgc"


	print_array();	

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 535 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 535 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 535 "uvarchar.pgc"
 
}

//prepared select using uvarchar array
void test_array_7()
{
    test("test_array_7 : prepared select using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 543 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 543 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 543 "uvarchar.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 546 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 546 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 546 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 547 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 547 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 547 "uvarchar.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 550 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 550 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 550 "uvarchar.pgc"

}

//prepared update using uvarchar array
void test_array_8()
{
    test("test_array_8 : prepared update using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 558 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 558 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 558 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 560 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 560 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 560 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 561 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 561 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 561 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 562 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 562 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 562 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 563 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 563 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 563 "uvarchar.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 566 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 566 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 566 "uvarchar.pgc"

	
	init_table_value();
}

//prepared delete using uvarchar array
void test_array_9()
{
    test("test_array_9 : prepared delete using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 576 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 576 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 576 "uvarchar.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 579 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 579 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 579 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 580 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 580 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 580 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 581 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 581 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 581 "uvarchar.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 584 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 584 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 584 "uvarchar.pgc"

	
	init_table_value();
}

//prepared insert using uvarchar array
void test_array_10()
{
    test("test_array_10 : prepared insert using uvarchar array");
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 594 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 594 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 594 "uvarchar.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, ?)");
#line 596 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 596 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 596 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"13",(long)2,(long)1,strlen("13"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 597 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 597 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 597 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"15",(long)2,(long)1,strlen("15"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 598 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 598 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 598 "uvarchar.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 599 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 599 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 599 "uvarchar.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 602 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 602 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 602 "uvarchar.pgc"

	
	init_table_value();
}

//Open cursor using uvarchar array
void test_array_11()
{
    test("test_array_11 : Open cursor using uvarchar array");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 613 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 613 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 613 "uvarchar.pgc"
   
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 614 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 614 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 614 "uvarchar.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 616 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 616 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 616 "uvarchar.pgc"

	/* declare cursor_array_11 cursor for $1 */
#line 617 "uvarchar.pgc"

	{ ECPGopen("cursor_array_11", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_array_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 618 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 618 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 618 "uvarchar.pgc"

	{ ECPGfetch("cursor_array_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 619 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 619 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 619 "uvarchar.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGclose("cursor_array_11", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_array_11", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 621 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 621 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 621 "uvarchar.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 622 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 622 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 622 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 623 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 623 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 623 "uvarchar.pgc"

}

//Fecth cursor into uvarchar array
void test_array_12()
{
    test("test_array_12 : Fecth cursor into uvarchar array");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 632 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 632 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 632 "uvarchar.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=3");
#line 633 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 633 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 633 "uvarchar.pgc"

	/* declare cursor_array_12 cursor for $1 */
#line 634 "uvarchar.pgc"

	{ ECPGopen("cursor_array_12", "stmt", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor_array_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 635 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 635 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 635 "uvarchar.pgc"

	{ ECPGfetch("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 636 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 636 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 636 "uvarchar.pgc"

	{ ECPGfetch("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[1]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 637 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 637 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 637 "uvarchar.pgc"

	{ ECPGfetch("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 638 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 638 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 638 "uvarchar.pgc"

	{ ECPGclose("cursor_array_12", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor_array_12", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 639 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 639 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 639 "uvarchar.pgc"

	
	print_array();
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 643 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 643 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 643 "uvarchar.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 644 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 644 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 644 "uvarchar.pgc"

}

//Insert array with L string inited
void test_array_13()
{
/* exec sql begin declare section */
    	

#line 651 "uvarchar.pgc"
  struct uvarchar_4  { int len; utext arr[ VAR_SIZE ]; }  uvarchar_array13 [ 3 ] ;
/* exec sql end declare section */
#line 652 "uvarchar.pgc"

    test("test_array_13 : Insert array with L string inited");
    
    memset((char*)&uvarchar_array13,0,sizeof(uvarchar_array13));
    
    memcpy((char*)uvarchar_array13[0].arr, L"太𠮷", sizeof(L"太𠮷"));
    uvarchar_array13[0].len = sizeof(L"太𠮷")/2;
    memcpy((char*)uvarchar_array13[1].arr, L"𠮷太", sizeof(L"𠮷太"));
    uvarchar_array13[1].len = sizeof(L"𠮷太")/2;
    memcpy((char*)uvarchar_array13[2].arr, L"太平", sizeof(L"太平"));
    uvarchar_array13[2].len = sizeof(L"太平")/2;
   
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_uvarchar,&(uvarchar_array13[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_4), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 664 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 664 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 664 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 21 )", 
	ECPGt_uvarchar,&(uvarchar_array13[1]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_4), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 665 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 665 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 665 "uvarchar.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 22 )", 
	ECPGt_uvarchar,&(uvarchar_array13[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_4), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 666 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 666 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 666 "uvarchar.pgc"

    
    init_var();
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 22 and count >= 20", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 669 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 669 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 669 "uvarchar.pgc"

    print_array();
    init_table_value();

}

//Buck insert uvarchar array into table
void test_buckinsert_1()
{
    test("test_buckinsert_1 : Buck insert uvarchar array into table");
    init_var();
    
 	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 681 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 681 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 681 "uvarchar.pgc"
 

    // truncate table
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate tb1", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);
#line 684 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 684 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 684 "uvarchar.pgc"

	
	//buck insert 
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "BULK_INSERT_V01_I0058R0000 insert into tb1 (Item , count) values ( $1  , $2  )", 
	ECPGt_uvarchar,(uvarchar_array),(long)VAR_SIZE,(long)ARRAY_SIZE,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(count_array),(long)1,(long)4,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOLT);
#line 687 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 687 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 687 "uvarchar.pgc"

	
    //check the results
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from tb1", ECPGt_EOIT, 
	ECPGt_int,&(total_tuples),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 690 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 690 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 690 "uvarchar.pgc"

    printf ("Total tuples in tb1 = %d\n", total_tuples);
    
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[0]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 693 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 693 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 693 "uvarchar.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_uvarchar,&(uvarchar_array[2]),(long)VAR_SIZE,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);
#line 696 "uvarchar.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 696 "uvarchar.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 696 "uvarchar.pgc"

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
	test_buckinsert_1();
}

int main() 
{
	ECPGdebug(1, stderr);
    if(test_init() !=0)
        return -1;
    
    test_all();
    //test_array_13();
    //test_var_1();
    test_finish();

	return 0;
}
