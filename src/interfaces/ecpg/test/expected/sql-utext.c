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
#include <wchar.h>

#line 1 "regression.h"






#line 5 "utext.pgc"


#define test(msg) printf("\n%s\n",msg)
#define VAR_SIZE  20
#define ARRAY_SIZE 4
#define P_VAR_SIZE 22


/* Following is UTF8 and UTF16/UTF32 characters mapping table */
/*太𠮷𠜱平洋𠱓大西洋印度洋北冰洋
utf16
0x592A 0xD842 0xDFB7 0xD841 0xDF31 0x5E73 0x6D0B 0xD843
0xDC53 0x5927 0x897F 0x6D0B 0x5370 0x5EA6 0x6D0B 0x5317
0x51B0 0x6D0B 0x0000,0x0000

utf32
0x592A  0x20BB7  0x20731  0x5E73  0x6D0B  0x20C53  0x5927  0x897F  
0x6D0B  0x5370  0x5EA6  0x6D0B  0x5317  0x51B0  0x6D0B 
*/

/*足球篮球羽毛球乒乓球橄榄球棒球冰球
utf16
0x8DB3,0x7403,0x7BEE,0x7403,0x7FBD,0x6BDB,0x7403,0x4E52,
0x4E53,0x7403,0x6A44,0x6984,0x7403,0x68D2,0x7403,0x51B0,
0x7403,0x0000,0x0000,0x0000

utf32
0x8DB3  0x7403  0x7BEE  0x7403  0x7FBD  0x6BDB  0x7403  0x4E52  
0x4E53  0x7403  0x6A44  0x6984  0x7403  0x68D2  0x7403  0x51B0  
0x7403
*/

/*世界杯每隔四年就会举行一次每次𠲖个球队
utf16
0x4E16 0x754C 0x676F 0x6BCF 0x9694 0x56DB 0x5E74 0x5C31 
0x4F1A 0x4E3E 0x884C 0x4E00 0x6B21 0x6BCF 0x6B21 0xD843 
0xDC96 0x4E2A 0x7403 0x961F

utf32
0x4E16  0x754C  0x676F  0x6BCF  0x9694  0x56DB  0x5E74  0x5C31  
0x4F1A  0x4E3E  0x884C  0x4E00  0x6B21  0x6BCF  0x6B21  0x20C96  
0x4E2A  0x7403  0x961F
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
void test_pvar_14(void);
void test_pvar_15(void);
void test_pvar_16(void);
void test_pvar_17(void);
void test_pvar_18(void);
void test_pvar_19(void);
void test_pp_var_1(void);
void test_pp_var_2(void);
void test_pp_var_3(void);
void test_desc_1(void);
void test_all(void);

void print_utext(utext *utext_var);
void print_utext_str(utext *utext_var);
void print_utext_ind(int utext_var_ind);
void print_utext_size(utext *utext_var,int size);
void print_array(void *array, int array_size, int var_size);
void print_array_with_index(int index);
int test_init(void);
void test_finish(void);
void init_table_value(void);
   
/* exec sql begin declare section */
           
           
			
	       
	     
	    
	
		
	   
		  
	     

	
			  
	     
	

#line 126 "utext.pgc"
 int utext_var_size = 20 ;
 
#line 127 "utext.pgc"
 int utext_array_size = 4 ;
 
#line 128 "utext.pgc"
 int count = 0 ;
 
#line 129 "utext.pgc"
 int total_tuples = 0 ;
 
#line 130 "utext.pgc"
 int i ;
 
#line 131 "utext.pgc"
 char char_var [ 10 ] = { 0xF0 , 0x90 , 0x90 , 0xB7 } ;
 
#line 133 "utext.pgc"
 utext utext_var [ VAR_SIZE ] ;
 
#line 134 "utext.pgc"
 utext utext_array [ ARRAY_SIZE ] [ VAR_SIZE ] ;
 
#line 135 "utext.pgc"
 utext utext_input_var [ 20 ] = { 0x592a , 0xd842 , 0xdfb7 , 0x0000 } ;
 
#line 136 "utext.pgc"
 utext * p_utext_var = NULL ;
 
#line 139 "utext.pgc"
 int utext_var_ind ;
 
#line 140 "utext.pgc"
 int count_array [ 4 ] = { 1 , 2 , 3 , 4 } ;
/* exec sql end declare section */
#line 142 "utext.pgc"


void print_utext(utext *utext_var)
{
    int i;
    printf("======print utext_var content======\n");    
	for(i=0; i<VAR_SIZE; i++)
	{
	    printf ("0x%08X  ", utext_var[i]);
	    if(i>6 && (i+1)%8==0)
	        printf("\n");
	}
    printf("\n======End utext_var content======\n");
}

void print_utext_str(utext *utext_var)
{
    int i=0;
    printf("======print utext_var_str content======\n");
    if (utext_var==0)
    {
        printf("utext_var is NULL\n");
        printf("\n======End utext_var_str content======\n");
        return;
    }
    while(utext_var[i] != 0)
	{
	    printf ("0x%08X  ", utext_var[i]);
	    if(i>6 && (i+1)%8==0)
	        printf("\n");
	    i++;
	    if(i>100)
	        break;
	}
    printf("\n======End utext_var_str content======\n");
}

void print_utext_size(utext *utext_var,int size)
{
    int i;
    printf("======print utext_var content======\n");    
	for(i=0; i<size; i++)
	{
	    printf ("0x%08X  ", utext_var[i]);
	    if(i>6 && (i+1)%8==0)
	        printf("\n");
	}
    printf("\n======End utext_var content======\n");
}

void print_utext_ind(int utext_var_ind)
{
	printf("utext_var_ind = %d\n",utext_var_ind);
}
void print_array(void *array, int array_size, int var_size)
{
    int i,j;

    for(i=0; i<array_size; i++)
    {
        utext *utext_array = &((utext*)array)[i*var_size];

        printf ("---->array[%d]:\n", i);

        for(j=0; j<var_size; j++)
        {
            printf ("0x%08X  ", utext_array[j]);
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
    
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); }
#line 227 "utext.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);}
#line 229 "utext.pgc"

	/* exec sql whenever sql_warning  sqlprint ; */
#line 230 "utext.pgc"

	/* exec sql whenever sqlerror  sqlprint ; */
#line 231 "utext.pgc"


    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "set client_encoding = 'UTF8'", ECPGt_EOIT, ECPGt_EORT);
#line 233 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 233 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 233 "utext.pgc"


    //initialization of test table
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "create table if not exists tb1 ( Item varchar , count integer )", ECPGt_EOIT, ECPGt_EORT);
#line 236 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 236 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 236 "utext.pgc"

	
	init_table_value();
	
	return 0;
}

void test_finish()
{
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "drop table tb1", ECPGt_EOIT, ECPGt_EORT);
#line 245 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 245 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 245 "utext.pgc"

	{ ECPGdisconnect(__LINE__, "ALL");
#line 246 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 246 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 246 "utext.pgc"

}

void init_table_value()
{
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "truncate tb1", ECPGt_EOIT, ECPGt_EORT);
#line 251 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 251 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 251 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋' , 1 )", ECPGt_EOIT, ECPGt_EORT);
#line 253 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 253 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 253 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '足球篮球羽毛球乒乓球橄榄球棒球冰球' , 2 )", ECPGt_EOIT, ECPGt_EORT);
#line 254 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 254 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 254 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '世界杯每隔四年就会举行一次每次𠲖个球队' , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 255 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 255 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 255 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( '亚洲欧洲非洲大洋洲北美洲南美洲南极洲没有北极洲' , 4 )", ECPGt_EOIT, ECPGt_EORT);
#line 256 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 256 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 256 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 ( count ) values ( 8 )", ECPGt_EOIT, ECPGt_EORT);
#line 257 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 257 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 257 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 ( count ) values ( 9 )", ECPGt_EOIT, ECPGt_EORT);
#line 258 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 258 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 258 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 ( count ) values ( 10 )", ECPGt_EOIT, ECPGt_EORT);
#line 259 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 259 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 259 "utext.pgc"

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
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 276 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 276 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 276 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 2", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 281 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 281 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 281 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 286 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 286 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 286 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 4", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 291 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 291 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 291 "utext.pgc"
 
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
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 305 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 305 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 305 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 306 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 306 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 306 "utext.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	count = 0;
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 310 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 310 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 310 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 311 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 311 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 311 "utext.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using utext var
void test_var_3()
{
    test("test_var_3: simple update using utext var");
    init_var();

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 321 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 321 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 321 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 2", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 322 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 322 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 322 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 3", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 323 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 323 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 323 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 324 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 324 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 324 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_table_value();
}

//simple delete using utext var
void test_var_4()
{
    test("test_var_4 : simple delete using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 336 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 336 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 336 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 337 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 337 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 337 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 338 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 338 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 338 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 341 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 341 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 341 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 342 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 342 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 342 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 343 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 343 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 343 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using utext var
void test_var_5()
{
    test("test_var_5 : simple insert using utext");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 355 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 355 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 355 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 356 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 356 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 356 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 357 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 357 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 357 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
    
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 361 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 361 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 361 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 362 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 362 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 362 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 363 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 363 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 363 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into utext var
void test_var_6()
{
    test("test_var_6 : prepared select into utext var");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=?");
#line 375 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 375 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 375 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 377 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 377 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 377 "utext.pgc"

	print_utext(utext_var);
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 380 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 380 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 380 "utext.pgc"

	print_utext(utext_var);

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 383 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 383 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 383 "utext.pgc"
 
}

//prepared select using utext var
void test_var_7()
{
    test("test_var_7 : prepared select using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 392 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 392 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 392 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 394 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 394 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 394 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 395 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 395 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 395 "utext.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 398 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 398 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 398 "utext.pgc"

}

//prepared update using utext var
void test_var_8()
{
    test("test_var_8 : prepared update using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 407 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 407 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 407 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 409 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 409 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 409 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 410 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 410 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 410 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 411 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 411 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 411 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 412 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 412 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 412 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 415 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 415 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 415 "utext.pgc"

	
	init_table_value();
}

//prepared delete using utext var
void test_var_9()
{
    test("test_var_9 : prepared delete using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 426 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 426 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 426 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 428 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 428 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 428 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 429 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 429 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 429 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 430 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 430 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 430 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 433 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 433 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 433 "utext.pgc"

	
	init_table_value();
}

//prepared insert using utext var
void test_var_10()
{
    test("test_var_10 : prepared insert using utext var");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 444 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 444 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 444 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, 13)");
#line 446 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 446 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 446 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 447 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 447 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 447 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 448 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 448 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 448 "utext.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 451 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 451 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 451 "utext.pgc"

	
	init_table_value();
}

//Open cursor using utext var
void test_var_11()
{
    test("test_var_11 : Open cursor using utext var");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 462 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 462 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 462 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 463 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 463 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 463 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 465 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 465 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 465 "utext.pgc"

	/* declare cursor_var_11 cursor for $1 */
#line 466 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_var_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 467 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 467 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 467 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_var_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 468 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 468 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 468 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_var_11", ECPGt_EOIT, ECPGt_EORT);
#line 470 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 470 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 470 "utext.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 471 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 471 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 471 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 472 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 472 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 472 "utext.pgc"

}

//Fecth cursor into utext var
void test_var_12()
{
    test("test_var_12 : Fecth cursor into utext var");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 481 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 481 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 481 "utext.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=1");
#line 482 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 482 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 482 "utext.pgc"

	/* declare cursor_var_12 cursor for $1 */
#line 483 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_var_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 484 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 484 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 484 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_var_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 485 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 485 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 485 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_var_12", ECPGt_EOIT, ECPGt_EORT);
#line 486 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 486 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 486 "utext.pgc"

	
	print_utext(utext_var);
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 490 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 490 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 490 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 491 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 491 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 491 "utext.pgc"

}

//simple insert utext var with L string inited
void test_var_13()
{
/* exec sql begin declare section */
		

#line 498 "utext.pgc"
 utext utext_local_var [] = L"足球篮球羽毛球" ;
/* exec sql end declare section */
#line 499 "utext.pgc"

    test("test_var_13 : simple insert utext var with L string inited");
	init_var();

	print_utext_str(utext_local_var);
	
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 16 )", 
	ECPGt_utext,(utext_local_var),(long)sizeof(L"足球篮球羽毛球")/4,(long)1,(sizeof(L"足球篮球羽毛球")/4)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 505 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 505 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 505 "utext.pgc"

    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 16", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 507 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 507 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 507 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
    init_table_value();
}

//Open cursor using utext var directly in WHERE Clause
void test_var_14()
{
/* exec sql begin declare section */
		

#line 518 "utext.pgc"
 utext utext_local_var [ 20 + 1 ] ;
/* exec sql end declare section */
#line 519 "utext.pgc"

    memset(utext_local_var,'a',sizeof(utext_local_var));
    
    test("test_var_14 : Open cursor using utext var directly in WHERE Clause");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 525 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 525 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 525 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 526 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 526 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 526 "utext.pgc"

	ECPGset_var( 0, ( utext_local_var ), __LINE__);\
 /* declare cursor_var_14 cursor for select count from tb1 where Item = $1  */
#line 527 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_var_14 cursor for select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 528 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 528 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 528 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_var_14", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 529 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 529 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 529 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_var_14", ECPGt_EOIT, ECPGt_EORT);
#line 532 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 532 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 532 "utext.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 534 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 534 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 534 "utext.pgc"

}

//Test utext_var working with NULL without indicator
void test_var_15()
{
/* exec sql begin declare section */
    	

#line 541 "utext.pgc"
 utext utext_local_var [ 20 + 1 ] ;
/* exec sql end declare section */
#line 542 "utext.pgc"

    test("test_var_15 : Test utext_var working with NULL without indicator");

    memset(utext_local_var,'a',sizeof(utext_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 546 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 546 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 546 "utext.pgc"

    print_utext(utext_local_var);
    
    memset(utext_local_var,0x00,sizeof(utext_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 550 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 550 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 550 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 551 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 551 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 551 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 552 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 552 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 552 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 553 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 553 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 553 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//utext_var working with NULL with indicator
void test_var_16()
{
/* exec sql begin declare section */
    	
         

#line 563 "utext.pgc"
 utext utext_local_var [ 20 + 1 ] ;
 
#line 564 "utext.pgc"
 int utext_var_ind = - 1 ;
/* exec sql end declare section */
#line 565 "utext.pgc"

    test("test_var_16 : Test utext_var working with NULL with indicator");

    memset(utext_local_var,'a',sizeof(utext_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 569 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 569 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 569 "utext.pgc"

    print_utext(utext_local_var);
    print_utext_ind(utext_var_ind);
    
    memset(utext_local_var,0x00,sizeof(utext_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 574 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 574 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 574 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 575 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 575 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 575 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,(utext_local_var),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 576 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 576 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 576 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 577 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 577 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 577 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}


//simple select into utext array 
void test_array_1()
{
    test("test_array_1 : simple select into utext array ");
    init_var();
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 590 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 590 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 590 "utext.pgc"


	print_array(utext_array,ARRAY_SIZE,VAR_SIZE);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 595 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 595 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 595 "utext.pgc"
 
	print_array(utext_array,ARRAY_SIZE,VAR_SIZE);
	init_var();
}

//simple select using utext array
void test_array_2()
{
    test("test_array_2 : simple select using array");
	init_var();
	    
 	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 606 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 606 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 606 "utext.pgc"
 

	count = 0;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 609 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 609 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 609 "utext.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	count = 0;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 613 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 613 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 613 "utext.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}

//simple update using utext array
void test_array_3()
{
    test("test_array_3 : simple update using utext array");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 623 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 623 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 623 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 1", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 625 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 625 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 625 "utext.pgc"


	count = 0;    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 628 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 628 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 628 "utext.pgc"

	printf ("find %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 0;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 632 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 632 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 632 "utext.pgc"

	printf ("find %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple delete using utext array
void test_array_4()
{
    test("test_array_4 : simple delete using utext array");
    init_var();

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 644 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 644 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 644 "utext.pgc"

    
    count = 100;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 647 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 647 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 647 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 648 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 648 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 648 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	count = 100;
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 652 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 652 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 652 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 653 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 653 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 653 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using utext array
void test_array_5()
{
    test("test_array_5 : simple insert using utext array");
    init_var();

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 665 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 665 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 665 "utext.pgc"

    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 11 )", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 667 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 667 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 667 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 668 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 668 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 668 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 671 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 671 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 671 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 672 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 672 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 672 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into utext array
void test_array_6()
{
    test("test_array_6 : prepared select into utext array");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=?");
#line 684 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 684 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 684 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"4",(long)1,(long)1,strlen("4"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 686 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 686 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 686 "utext.pgc"


    print_array(utext_array,ARRAY_SIZE,VAR_SIZE);

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 690 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 690 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 690 "utext.pgc"
 
}

//prepared select using utext array
void test_array_7()
{
    test("test_array_7 : prepared select using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 698 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 698 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 698 "utext.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 701 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 701 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 701 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 702 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 702 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 702 "utext.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 705 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 705 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 705 "utext.pgc"

}

//prepared update using utext array
void test_array_8()
{
    test("test_array_8 : prepared update using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 713 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 713 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 713 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 715 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 715 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 715 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 716 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 716 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 716 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 717 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 717 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 717 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 718 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 718 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 718 "utext.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 721 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 721 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 721 "utext.pgc"

	
	init_table_value();
}

//prepared delete using utext array
void test_array_9()
{
    test("test_array_9 : prepared delete using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 731 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 731 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 731 "utext.pgc"

    
    count = 0;
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 734 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 734 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 734 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 735 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 735 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 735 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 736 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 736 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 736 "utext.pgc"


	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 739 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 739 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 739 "utext.pgc"

	
	init_table_value();
}

//prepared insert using utext array
void test_array_10()
{
    test("test_array_10 : prepared insert using utext array");
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 749 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 749 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 749 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, ?)");
#line 751 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 751 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 751 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"13",(long)2,(long)1,strlen("13"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 752 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 752 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 752 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"15",(long)2,(long)1,strlen("15"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 753 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 753 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 753 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 754 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 754 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 754 "utext.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 757 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 757 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 757 "utext.pgc"

	
	init_table_value();
}

//Open cursor using utext array
void test_array_11()
{
    test("test_array_11 : Open cursor using utext array");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 768 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 768 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 768 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 769 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 769 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 769 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 771 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 771 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 771 "utext.pgc"

	/* declare cursor_array_11 cursor for $1 */
#line 772 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_array_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 773 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 773 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 773 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 774 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 774 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 774 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_array_11", ECPGt_EOIT, ECPGt_EORT);
#line 776 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 776 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 776 "utext.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 777 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 777 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 777 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 778 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 778 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 778 "utext.pgc"

}

//Fecth cursor into utext array
void test_array_12()
{
    test("test_array_12 : Fecth cursor into utext array");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 787 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 787 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 787 "utext.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count<=3");
#line 788 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 788 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 788 "utext.pgc"

	/* declare cursor_array_12 cursor for $1 */
#line 789 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_array_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 790 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 790 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 790 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 791 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 791 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 791 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[1]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 792 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 792 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 792 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_12", ECPGt_EOIT, 
	ECPGt_utext,(utext_array[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 793 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 793 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 793 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_array_12", ECPGt_EOIT, ECPGt_EORT);
#line 794 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 794 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 794 "utext.pgc"

	
    print_array(utext_array,ARRAY_SIZE,VAR_SIZE);
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 798 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 798 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 798 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 799 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 799 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 799 "utext.pgc"

}

//Insert array with L string inited
void test_array_13()
{
/* exec sql begin declare section */
    	

#line 806 "utext.pgc"
 utext utext_array13 [ 4 ] [ VAR_SIZE ] = { L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋" , L"足球篮球羽毛球乒乓球橄榄球棒球冰球" , L"世界杯每隔四年就会举行一次" } ;
/* exec sql end declare section */
#line 807 "utext.pgc"


    test("test_array_13 : Insert array with L string inited");
    init_var();


    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,(utext_array13[0]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 813 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 813 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 813 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 21 )", 
	ECPGt_utext,(utext_array13[1]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 814 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 814 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 814 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 22 )", 
	ECPGt_utext,(utext_array13[2]),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 815 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 815 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 815 "utext.pgc"

    
    init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 22 and count >= 20", ECPGt_EOIT, 
	ECPGt_utext,(utext_array),(long)VAR_SIZE,(long)ARRAY_SIZE,(VAR_SIZE)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 818 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 818 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 818 "utext.pgc"

    print_array(utext_array,3,VAR_SIZE);
    init_table_value();
}

//Open cursor using utext array directly in WHERE Clause
void test_array_14()
{
/* exec sql begin declare section */
    	

#line 827 "utext.pgc"
 utext utext_local_array [ 4 ] [ 20 + 1 ] ;
/* exec sql end declare section */
#line 828 "utext.pgc"


    test("test_array_14 : Open cursor using utext array directly in WHERE Clause");
    memset(utext_local_array,'a',sizeof(utext_local_array));

 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 833 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 833 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 833 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,(utext_local_array),(long)20 + 1,(long)4,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 834 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 834 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 834 "utext.pgc"


	ECPGset_var( 1, ( utext_local_array[2] ), __LINE__);\
 /* declare cursor_array_14 cursor for select count from tb1 where Item = $1  */
#line 836 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_array_14 cursor for select count from tb1 where Item = $1 ", 
	ECPGt_utext,(utext_local_array[2]),(long)20 + 1,(long)1,(20 + 1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 837 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 837 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 837 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_array_14", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 838 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 838 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 838 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_array_14", ECPGt_EOIT, ECPGt_EORT);
#line 840 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 840 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 840 "utext.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 842 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 842 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 842 "utext.pgc"

}


//utext array working with NULL without using indicator
void test_array_15()
{
/* exec sql begin declare section */
    	

#line 850 "utext.pgc"
 utext utext_local_array [ 3 ] [ 20 ] ;
/* exec sql end declare section */
#line 851 "utext.pgc"

    test("test_array_15 : Test utext array working with NULL without using indicator");
    memset(utext_local_array,'a',sizeof(utext_local_array));
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count >= 8 and count <= 10", ECPGt_EOIT, 
	ECPGt_utext,(utext_local_array),(long)20,(long)3,(20)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 855 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 855 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 855 "utext.pgc"

	
    print_array((void**)utext_local_array,3,20);
    
    memset(utext_local_array,0x00,sizeof(utext_local_array));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,(utext_local_array[0]),(long)20,(long)1,(20)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 860 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 860 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 860 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,(utext_local_array[1]),(long)20,(long)1,(20)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 861 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 861 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 861 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,(utext_local_array[2]),(long)20,(long)1,(20)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 862 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 862 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 862 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 863 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 863 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 863 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//utext array working with NULL using indicator
void test_array_16()
{
/* exec sql begin declare section */
    	
         

#line 873 "utext.pgc"
 utext utext_local_array [ 3 ] [ 20 ] ;
 
#line 874 "utext.pgc"
 int utext_array_ind [ 3 ] ;
/* exec sql end declare section */
#line 875 "utext.pgc"

    test("test_array_16 : Test utext array working with NULL using indicator");
    memset(utext_local_array,'a',sizeof(utext_local_array));
    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count >= 8 and count <= 10", ECPGt_EOIT, 
	ECPGt_utext,(utext_local_array),(long)20,(long)3,(20)*sizeof(utext), 
	ECPGt_int,(utext_array_ind),(long)1,(long)3,sizeof(int), ECPGt_EORT);
#line 879 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 879 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 879 "utext.pgc"

	
    print_array((void**)utext_local_array,3,20);
    print_utext_ind(utext_array_ind[0]);
    print_utext_ind(utext_array_ind[1]);
    print_utext_ind(utext_array_ind[2]);
    
    memset(utext_local_array,0x00,sizeof(utext_local_array));
    utext_array_ind[0]=-1;
    utext_array_ind[1]=-1;
    utext_array_ind[2]=-1;
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,(utext_local_array[0]),(long)20,(long)1,(20)*sizeof(utext), 
	ECPGt_int,&(utext_array_ind[0]),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 890 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 890 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 890 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,(utext_local_array[1]),(long)20,(long)1,(20)*sizeof(utext), 
	ECPGt_int,&(utext_array_ind[1]),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 891 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 891 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 891 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,(utext_local_array[2]),(long)20,(long)1,(20)*sizeof(utext), 
	ECPGt_int,&(utext_array_ind[2]),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 892 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 892 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 892 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 893 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 893 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 893 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}


//simple select into utext pointer
void test_pvar_1()
{
    test("test_pvar_1 : simple select into utext pointer");
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 904 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 904 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 904 "utext.pgc"
 
	print_utext(p_utext_var);
	print_utext_ind(utext_var_ind);
	init_var();

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 2", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 909 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 909 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 909 "utext.pgc"
 
	print_utext(p_utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 914 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 914 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 914 "utext.pgc"
 
	print_utext(p_utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 4", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 919 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 919 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 919 "utext.pgc"
 
	print_utext(p_utext_var);
	print_utext_ind(utext_var_ind);
	init_var();

    init_table_value();
}


//simple select using utext pointer
void test_pvar_2()
{
    test("test_pvar_2 : simple select using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 934 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 934 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 934 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 935 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 935 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 935 "utext.pgc"

	printf ("count=%d for '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);

	init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 939 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 939 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 939 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 940 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 940 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 940 "utext.pgc"

	printf ("count=%d for '世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
}


//simple update using utext pointer
void test_pvar_3()
{
    test("test_pvar_3 : simple update using utext pointer");
    init_var();
    
    count = 0;
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 952 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 952 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 952 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 2", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 953 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 953 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 953 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "update tb1 set Item = $1  where count = 3", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 954 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 954 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 954 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 955 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 955 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 955 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_table_value();
}

//simple delete using utext pointer 
void test_pvar_4()
{
    test("test_pvar_4 : simple delete using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 967 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 967 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 967 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 968 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 968 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 968 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 969 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 969 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 969 "utext.pgc"

	printf ("found %d rows for Item='太𠮷𠜱平洋𠱓大西洋印度洋北冰洋'\n", count);
	
	init_var();
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 973 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 973 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 973 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "delete from tb1 where Item = $1 ", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 974 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 974 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 974 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 975 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 975 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 975 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//simple insert using utext pointer
void test_pvar_5()
{
    test("test_pvar_5 : simple insert using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 987 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 987 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 987 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 13 )", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 988 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 988 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 988 "utext.pgc"


	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 990 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 990 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 990 "utext.pgc"

	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	
	init_table_value();
}

//prepared select into utext pointer
void test_pvar_6()
{
    test("test_pvar_6 : prepared select into utext pointer");
    init_var();
    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=?");
#line 1002 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1002 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1002 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1004 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1004 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1004 "utext.pgc"

	print_utext(p_utext_var);
	
	init_var();
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_const,"3",(long)1,(long)1,strlen("3"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1008 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1008 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1008 "utext.pgc"

	print_utext(p_utext_var);
    init_table_value();
    
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 1012 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1012 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1012 "utext.pgc"
 
    
}

//prepared select using utext pointer
void test_pvar_7()
{
    test("test_pvar_7 : prepared select using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1022 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1022 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1022 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 1024 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1024 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1024 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1025 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1025 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1025 "utext.pgc"

	
	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 1028 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1028 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1028 "utext.pgc"

}

//prepared update using utext pointer
void test_pvar_8()
{
    test("test_pvar_8 : prepared update using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1037 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1037 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1037 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "UPDATE tb1 SET Item=? WHERE Count=?");
#line 1039 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1039 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1039 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1040 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1040 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1040 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1041 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1041 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1041 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1042 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1042 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1042 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 1045 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1045 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1045 "utext.pgc"

	
	init_table_value();
}

//prepared delete using utext pointer
void test_pvar_9()
{
    test("test_pvar_9 : prepared delete using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1056 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1056 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1056 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "DELETE FROM tb1 WHERE Item=?");
#line 1058 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1058 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1058 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1059 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1059 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1059 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1060 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1060 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1060 "utext.pgc"


	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 1063 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1063 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1063 "utext.pgc"

	
	init_table_value();
}

//prepared insert using utext pointer
void test_pvar_10()
{
    test("test_pvar_10 : prepared insert using utext pointer");
    init_var();
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1074 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1074 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1074 "utext.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "INSERT INTO tb1 values (?, 13)");
#line 1076 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1076 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1076 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_execute, "stmt", 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1077 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1077 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1077 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item = '世界杯每隔四年就会举行一次每次𠲖个球队'", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1078 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1078 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1078 "utext.pgc"

	
	printf ("found %d rows for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 1081 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1081 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1081 "utext.pgc"

	
	init_table_value();
}

//Open cursor using utext pointer
void test_pvar_11()
{
    test("test_pvar_11 : Open cursor using utext pointer");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 1092 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1092 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1092 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1093 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1093 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1093 "utext.pgc"

    
	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Count FROM tb1 WHERE Item=?");
#line 1095 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1095 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1095 "utext.pgc"

	/* declare cursor_pvar_11 cursor for $1 */
#line 1096 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_pvar_11 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1097 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1097 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1097 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_pvar_11", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1098 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1098 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1098 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_pvar_11", ECPGt_EOIT, ECPGt_EORT);
#line 1100 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1100 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1100 "utext.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 1101 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1101 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1101 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 1102 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1102 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1102 "utext.pgc"

}

//Fecth cursor into utext pointer
void test_pvar_12()
{
    test("test_pvar_12 : Fecth cursor into utext pointer");
    init_var();
     
    { ECPGsetcommit(__LINE__, "off", NULL);
#line 1111 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1111 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1111 "utext.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "stmt", "SELECT Item FROM tb1 WHERE Count=1");
#line 1112 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1112 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1112 "utext.pgc"

	/* declare cursor_pvar_12 cursor for $1 */
#line 1113 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_pvar_12 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1114 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1114 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1114 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_pvar_12", ECPGt_EOIT, 
	ECPGt_utext,&(p_utext_var),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1115 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1115 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1115 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_pvar_12", ECPGt_EOIT, ECPGt_EORT);
#line 1116 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1116 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1116 "utext.pgc"

	
	print_utext(p_utext_var);
		
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt");
#line 1120 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1120 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1120 "utext.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 1121 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1121 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1121 "utext.pgc"

}

//simple insert utext pointer with L string inited
void test_pvar_13()
{
/* exec sql begin declare section */
		

#line 1128 "utext.pgc"
 utext * utext_local_pvar = L"太𠮷𠜱平洋𠱓大西洋印度洋北冰洋" ;
/* exec sql end declare section */
#line 1129 "utext.pgc"

    test("test_pvar_13 : simple insert utext pointer with L string inited");
	init_var();
	
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 16 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1133 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1133 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1133 "utext.pgc"

    
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 16", ECPGt_EOIT, 
	ECPGt_utext,(utext_var),(long)VAR_SIZE,(long)1,(VAR_SIZE)*sizeof(utext), 
	ECPGt_int,&(utext_var_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 1135 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1135 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1135 "utext.pgc"
 
	print_utext(utext_var);
	print_utext_ind(utext_var_ind);
	init_var();
    init_table_value();
}

//Open cursor using utext var directly in WHERE Clause
void test_pvar_14()
{
/* exec sql begin declare section */
     

#line 1146 "utext.pgc"
 utext * utext_local_pvar ;
/* exec sql end declare section */
#line 1147 "utext.pgc"

    utext_local_pvar = (utext*)malloc(P_VAR_SIZE*sizeof(utext));
    memset(utext_local_pvar,'a',P_VAR_SIZE*sizeof(utext));
    
    test("test_pvar_14 : Open cursor using utext var directly in WHERE Clause");
    init_var();
 
 	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 1154 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1154 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1154 "utext.pgc"
   
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 3", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1155 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1155 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1155 "utext.pgc"


	ECPGset_var( 2, &( utext_local_pvar ), __LINE__);\
 /* declare cursor_pvar_14 cursor for select count from tb1 where Item = $1  */
#line 1157 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "declare cursor_pvar_14 cursor for select count from tb1 where Item = $1 ", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1158 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1158 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1158 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "fetch cursor_pvar_14", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1159 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1159 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1159 "utext.pgc"

	printf ("count=%d for Item='世界杯每隔四年就会举行一次每次𠲖个球队'\n", count);
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "close cursor_pvar_14", ECPGt_EOIT, ECPGt_EORT);
#line 1161 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1161 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1161 "utext.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 1163 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1163 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1163 "utext.pgc"

}


//utext pointer working with NULL without using indicator
void test_pvar_15()
{
/* exec sql begin declare section */
    	

#line 1171 "utext.pgc"
 utext * utext_local_pvar ;
/* exec sql end declare section */
#line 1172 "utext.pgc"

    utext_local_pvar = (utext*)malloc(P_VAR_SIZE*sizeof(utext));
    memset(utext_local_pvar,'a',P_VAR_SIZE*sizeof(utext));
    
    test("test_pvar_15 : Test utext pointer working with NULL without using indicator");

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1178 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1178 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1178 "utext.pgc"

    
    print_utext(utext_local_pvar);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1182 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1182 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1182 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1183 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1183 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1183 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1184 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1184 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1184 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1185 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1185 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1185 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//utext pointer working with NULL using indicator
void test_pvar_16()
{
/* exec sql begin declare section */
    	
           

#line 1195 "utext.pgc"
 utext * utext_local_pvar ;
 
#line 1196 "utext.pgc"
 int utext_pvar_ind = - 1 ;
/* exec sql end declare section */
#line 1197 "utext.pgc"

    utext_local_pvar = (utext*)malloc(P_VAR_SIZE*sizeof(utext));
    memset(utext_local_pvar,'a',P_VAR_SIZE*sizeof(utext));
    
    test("test_pvar_16 : Test utext pointer working with NULL using indicator");

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 1203 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1203 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1203 "utext.pgc"

    
    print_utext(utext_local_pvar);
    print_utext_ind(utext_pvar_ind);
    
    memset(utext_local_pvar,0,P_VAR_SIZE*sizeof(utext));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 1209 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1209 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1209 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 1210 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1210 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1210 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 1211 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1211 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1211 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1212 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1212 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1212 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//utext uninitialized pointer getting and setting data
void test_pvar_17()
{
/* exec sql begin declare section */
    	
        

#line 1222 "utext.pgc"
 utext * utext_local_pvar = 0 ;
 
#line 1223 "utext.pgc"
 char local_str [ 50 ] ;
/* exec sql end declare section */
#line 1224 "utext.pgc"

   
    test("test_pvar_17 : Test utext uninitialized pointer getting and setting data");

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1228 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1228 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1228 "utext.pgc"

    
    print_utext_str(utext_local_pvar);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1232 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1232 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1232 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 18", ECPGt_EOIT, 
	ECPGt_char,(local_str),(long)50,(long)1,(50)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1233 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1233 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1233 "utext.pgc"

	printf ("Find Item = %s where Count=18\n", local_str);
    
    init_table_value();
}

//utext uninitialized pointer working with NULL without using indicator
void test_pvar_18()
{
/* exec sql begin declare section */
    	

#line 1243 "utext.pgc"
 utext * utext_local_pvar = 0 ;
/* exec sql end declare section */
#line 1244 "utext.pgc"

   
    test("test_pvar_18 : Test utext uninitialized pointer working with NULL without using indicator");

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1248 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1248 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1248 "utext.pgc"

    
    print_utext_size(utext_local_pvar,1);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1252 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1252 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1252 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1253 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1253 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1253 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1254 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1254 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1254 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1255 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1255 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1255 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//utext uninitialized pointer working with NULL using indicator
void test_pvar_19()
{
/* exec sql begin declare section */
    	
           

#line 1265 "utext.pgc"
 utext * utext_local_pvar = 0 ;
 
#line 1266 "utext.pgc"
 int utext_pvar_ind = - 1 ;
/* exec sql end declare section */
#line 1267 "utext.pgc"

   
    test("test_pvar_19 : Test utext uninitialized pointer working with NULL using indicator");

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 8", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 1271 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1271 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1271 "utext.pgc"

    
    print_utext_size(utext_local_pvar,1);
    print_utext_ind(utext_pvar_ind);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 1276 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1276 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1276 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 1277 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1277 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1277 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_int,&(utext_pvar_ind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);
#line 1278 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1278 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1278 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1279 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1279 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1279 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);
    
    init_table_value();
}

//Test select into host varaibles from descriptor
void test_desc_1()
{
	/* exec sql begin declare section */
	 
	 
	 
	   
	
#line 1289 "utext.pgc"
 utext utext_local_var [ 20 ] ;
 
#line 1290 "utext.pgc"
 utext * utext_local_pvar = 0 ;
 
#line 1291 "utext.pgc"
 utext ** utext_local_ppvar = 0 ;
 
#line 1292 "utext.pgc"
 char desc1 [ 8 ] = "outdesc" ;
/* exec sql end declare section */
#line 1293 "utext.pgc"


//	ECPGdebug(1, stderr);

	ECPGallocate_desc(__LINE__, "indesc");
#line 1297 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1297 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 1297 "utext.pgc"

	ECPGallocate_desc(__LINE__, (desc1));
#line 1298 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1298 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 1298 "utext.pgc"

	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item , count from tb1 where count = 1", ECPGt_EOIT, 
	ECPGt_descriptor, (desc1), 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1300 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1300 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1300 "utext.pgc"
 
	
	{ ECPGget_desc(__LINE__, (desc1), 1,ECPGd_data,
	ECPGt_utext,(utext_local_var),(long)20,(long)1,(20)*sizeof(utext), ECPGd_EODT);

#line 1302 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1302 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1302 "utext.pgc"

	print_utext(utext_local_var);

	{ ECPGget_desc(__LINE__, (desc1), 1,ECPGd_data,
	ECPGt_utext,&(utext_local_pvar),(long)0,(long)1,(1)*sizeof(utext), ECPGd_EODT);

#line 1305 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1305 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1305 "utext.pgc"

	print_utext_str(utext_local_pvar);
	
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item , count from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_descriptor, (desc1), 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1309 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1309 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1309 "utext.pgc"
 
	{ ECPGget_desc(__LINE__, (desc1), 1,ECPGd_data,
	ECPGt_utext,&(utext_local_ppvar),(long)0,(long)0,(1)*sizeof(utext), ECPGd_EODT);

#line 1310 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1310 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1310 "utext.pgc"

	print_utext_str(utext_local_ppvar[0]);
    print_utext_str(utext_local_ppvar[1]);
	print_utext_str(utext_local_ppvar[2]);
	
	
	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item , count from tb1 where count <= 10 and count >= 8", ECPGt_EOIT, 
	ECPGt_descriptor, (desc1), 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1316 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1316 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1316 "utext.pgc"
 
	{ ECPGget_desc(__LINE__, (desc1), 1,ECPGd_data,
	ECPGt_utext,&(utext_local_ppvar),(long)0,(long)0,(1)*sizeof(utext), ECPGd_EODT);

#line 1317 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1317 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1317 "utext.pgc"

	print_utext_str(utext_local_ppvar[0]);
    print_utext_str(utext_local_ppvar[1]);
	print_utext_str(utext_local_ppvar[2]);
	
		
	ECPGdeallocate_desc(__LINE__, "indesc");
#line 1323 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1323 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 1323 "utext.pgc"

	ECPGdeallocate_desc(__LINE__, (desc1));
#line 1324 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1324 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 1324 "utext.pgc"

}


//Test uninitialized pointer to pointer utext_var without initialize
void test_pp_var_1()
{
/* exec sql begin declare section */
       
        
        

#line 1332 "utext.pgc"
 utext ** utext_local_ppvar = 0 ;
 
#line 1333 "utext.pgc"
 int * utext_local_ppvar_ind = 0 ;
 
#line 1334 "utext.pgc"
 char utext_local_char [ 80 ] ;
/* exec sql end declare section */
#line 1335 "utext.pgc"

    test("test_pp_var_1 : Test pointer to pointer utext_var without initialize");

    //memset(utext_local_var,'a',sizeof(utext_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count <= 3", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_ppvar),(long)0,(long)0,(1)*sizeof(utext), 
	ECPGt_int,&(utext_local_ppvar_ind),(long)1,(long)0,sizeof(int), ECPGt_EORT);
#line 1339 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1339 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1339 "utext.pgc"

    print_utext_str(utext_local_ppvar[0]);
    print_utext_ind(utext_local_ppvar_ind[0]);
    
    print_utext_str(utext_local_ppvar[1]);
    print_utext_ind(utext_local_ppvar_ind[1]);
    
    print_utext_str(utext_local_ppvar[2]);
    print_utext_ind(utext_local_ppvar_ind[2]);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 28 )", 
	ECPGt_utext,&(utext_local_ppvar[1]),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1349 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1349 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1349 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count = 28", ECPGt_EOIT, 
	ECPGt_char,(utext_local_char),(long)80,(long)1,(80)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1350 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1350 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1350 "utext.pgc"

    printf("%s\n",utext_local_char);

    init_table_value();
}


//Test uninitialized pointer to pointer utext_var working with NULL without indicator
void test_pp_var_2()
{
/* exec sql begin declare section */
       

#line 1361 "utext.pgc"
 utext ** utext_local_ppvar = 0 ;
/* exec sql end declare section */
#line 1362 "utext.pgc"

    test("test_pp_var_2 : Test uninitialized pointer to pointer utext_var working with NULL without indicator");

    //memset(utext_local_var,'a',sizeof(utext_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count >= 8 and count <= 10", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_ppvar),(long)0,(long)0,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1366 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1366 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1366 "utext.pgc"

    print_utext_str(utext_local_ppvar[0]);
    print_utext_str(utext_local_ppvar[1]);
    print_utext_str(utext_local_ppvar[2]);

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,&(utext_local_ppvar[0]),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1371 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1371 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1371 "utext.pgc"
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,&(utext_local_ppvar[1]),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1372 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1372 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1372 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,&(utext_local_ppvar[2]),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1373 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1373 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1373 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1374 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1374 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1374 "utext.pgc"

	printf ("found %d rows for Item being NULL\n", count);

    init_table_value();
}

//Test uninitialized pointer to pointer utext_var working with NULL with indicator
void test_pp_var_3()
{
/* exec sql begin declare section */
       
        

#line 1384 "utext.pgc"
 utext ** utext_local_ppvar = 0 ;
 
#line 1385 "utext.pgc"
 int * utext_local_ppvar_ind = 0 ;
/* exec sql end declare section */
#line 1386 "utext.pgc"

    test("test_pp_var_2 : Test uninitialized pointer to pointer utext_var working with NULL without indicator");

    //memset(utext_local_var,'a',sizeof(utext_local_var));
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select Item from tb1 where count >= 8 and count <= 10", ECPGt_EOIT, 
	ECPGt_utext,&(utext_local_ppvar),(long)0,(long)0,(1)*sizeof(utext), 
	ECPGt_int,&(utext_local_ppvar_ind),(long)1,(long)0,sizeof(int), ECPGt_EORT);
#line 1390 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1390 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1390 "utext.pgc"

    print_utext_str(utext_local_ppvar[0]);
    print_utext_ind(utext_local_ppvar_ind[0]);
    
    print_utext_str(utext_local_ppvar[1]);
    print_utext_ind(utext_local_ppvar_ind[1]);
    
    print_utext_str(utext_local_ppvar[2]);
    print_utext_ind(utext_local_ppvar_ind[2]);
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 18 )", 
	ECPGt_utext,&(utext_local_ppvar[0]),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1400 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1400 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1400 "utext.pgc"
    
    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 19 )", 
	ECPGt_utext,&(utext_local_ppvar[1]),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1401 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1401 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1401 "utext.pgc"

    { ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "insert into tb1 values ( $1  , 20 )", 
	ECPGt_utext,&(utext_local_ppvar[2]),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 1402 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1402 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1402 "utext.pgc"

	{ ECPGdo(__LINE__, 0, 0, NULL, 0, ECPGst_normal, "select count ( * ) from tb1 where Item is null", ECPGt_EOIT, 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 1403 "utext.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 1403 "utext.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 1403 "utext.pgc"

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
    test_array_14();
    test_array_15();
    test_array_16();
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
    test_pvar_14();
    test_pvar_15();
    test_pvar_16();
    test_pvar_17();
    test_pvar_18();
    test_pvar_19();
    test_pp_var_1();
    test_pp_var_2();
    test_pp_var_3();
}

int main(int argc, char *argv[])
{
//	ECPGdebug(1, stderr);
    if(test_init() !=0)
        return -1;

	test_all();
    test_finish();

	return 0;
}
