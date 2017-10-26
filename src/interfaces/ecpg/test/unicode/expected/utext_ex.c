/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#define ECPG_ENABLE_UTEXT 1
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "utext_ex.pgc"
#include <stdio.h>


#line 1 "./../regression.h"






#line 3 "utext_ex.pgc"


void print_utext(char *var_name, utext *utext_var, int var_size);
void print_utext_array(char *var_name, utext *utext_var, int var_size, int index);
void print_nchar(char *utext_var, int var_size);

#define print_ret print_utext("employee",employee,40); \
print_utext("address", address.arr, address.len)

/* exec sql begin declare section */
   

  
  
  

   

 
 
 

#line 13 "utext_ex.pgc"
 char char_ename [ 3 ] [ 40 ] = { "test ok" , "测试 通过" , "太𠮷𠜱" } ;
 
#line 15 "utext_ex.pgc"
 char employee_1 [ 40 ] = "test ok" ;
 
#line 16 "utext_ex.pgc"
 char employee_2 [ 40 ] = "测试 通过" ;
 
#line 17 "utext_ex.pgc"
 char employee_3 [ 40 ] = "太𠮷𠜱" ;
 
#line 19 "utext_ex.pgc"
 char char_addr [ 3 ] [ 40 ] = { "1 sydney, NSW" , "澳大利亚悉尼1号" , "世界杯" } ;
 
#line 21 "utext_ex.pgc"
 char address_1 [ 40 ] = "1 sydney, NSW" ;
 
#line 22 "utext_ex.pgc"
 char address_2 [ 40 ] = "澳大利亚悉尼1号" ;
 
#line 23 "utext_ex.pgc"
 char address_3 [ 40 ] = "世界杯" ;
/* exec sql end declare section */
#line 24 "utext_ex.pgc"


void print_utext(char *var_name, utext *utext_var, int var_size)
{
    int i;
    printf("======print %s content======\n",var_name);
    for(i=0; i<var_size; i++)
    {
      printf ("0x%04X  ", utext_var[i]);
      if(i>6 && (i+1)%8==0)
          printf("\n");
    }
    printf("\n");
}

void print_utext_array(char *var_name, utext *utext_var, int var_size, int index)
{
    int i;
    printf("======print %s[%d] content======\n",var_name,index);
    for(i=0; i<var_size; i++)
    {
      printf ("0x%04X  ", utext_var[i]);
      if(i>6 && (i+1)%8==0)
          printf("\n");
    }
    printf("\n");
}

void print_nchar(char *utext_var, int var_size)
{
    printf("nchar is : %s\n",utext_var);
    int i;
    printf("======print utext_var content======\n");
    for(i=0; i<var_size; i++)
    {
        printf ("0x%04X  ", utext_var[i]);
        if(i>6 && (i+1)%8==0)
            printf("\n");
    }
    printf("\n======End utext_var content======\n");
}

void create_table()
{
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table if not exists emp ( id int , ename char ( 20 ) , address nvarchar ( 50 ) )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 68 "utext_ex.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate table emp", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 69 "utext_ex.pgc"

    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table if not exists emp_bk ( id int , ename char ( 20 ) , address nvarchar ( 50 ) )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 71 "utext_ex.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate table emp_bk", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 72 "utext_ex.pgc"

}

void init_table()
{
    /* UTF8 is the database character set */
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp ( id , ename , address ) values ( 1 , 'test ok' , '1 sydney, NSW' )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 78 "utext_ex.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp ( id , ename , address ) values ( 2 , '测试 通过' , '澳大利亚悉尼1号' )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 79 "utext_ex.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp ( id , ename , address ) values ( 3 , '太𠮷𠜱' , '世界杯' )", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 80 "utext_ex.pgc"

}

/*
 Test utext host variable
*/
void testcase1()
{
    /* exec sql begin declare section */
       
       
       
       
             /* define Unicode host variable */
       /* define a variable length Unicode host variable */
    
#line 89 "utext_ex.pgc"
 int total_tuples = 0 ;
 
#line 90 "utext_ex.pgc"
 int i = 0 ;
 
#line 91 "utext_ex.pgc"
 int id = 0 ;
 
#line 92 "utext_ex.pgc"
 int id2 = 0 ;
 
#line 93 "utext_ex.pgc"
 utext employee [ 41 ] ;
 
#line 94 "utext_ex.pgc"
  struct uvarchar_1  { int len; utext arr[ 101 ]; }  address ;
/* exec sql end declare section */
#line 95 "utext_ex.pgc"


    printf("\n**************Start testcase(%d)**************\n",1);
    
    memset(employee,'a',sizeof(employee));
    memset((void*)&address,'a',sizeof(address));
    address.len = 0;
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate emp_bk", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 103 "utext_ex.pgc"
   
    
    for(i=1;i<=3;i++)
    {
        /* Database character set converted to Unicode */
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ename , address from emp where id = $1 ", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(employee),(long)41,(long)1,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 108 "utext_ex.pgc"

        print_ret;
        /* Unicode converted to Database character */
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp_bk ( id , ename , address ) values ( $1  , $2  , $3  )", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,(employee),(long)41,(long)1,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 111 "utext_ex.pgc"
    
    }
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from emp_bk", ECPGt_EOIT, 
	ECPGt_int,&(total_tuples),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 114 "utext_ex.pgc"

    printf("total_tuples = %d in emp_bk\n",total_tuples);
    
    
    for(i=3;i>0;i--)
    {
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ename , address from emp where id = $1 ", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,(employee),(long)41,(long)1,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 120 "utext_ex.pgc"

        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select id from emp_bk where ename = $1  and address = $2 ", 
	ECPGt_utext,(employee),(long)41,(long)1,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 121 "utext_ex.pgc"

        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select id from emp_bk where ename = $1  and address = $2 ", 
	ECPGt_char,(char_ename[i-1]),(long)40,(long)1,(40)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(char_addr[i-1]),(long)40,(long)1,(40)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 122 "utext_ex.pgc"

        printf("id[%d] = id2[%d] in emp_bk\n",id,id2);    
    }
}

/*
 Test utext array
*/
void testcase2()
{
    /* exec sql begin declare section */
       
       
       
       
       
             /* define Unicode host variable */
       /* define a variable length Unicode host variable */
    
#line 133 "utext_ex.pgc"
 int total_tuples = 0 ;
 
#line 134 "utext_ex.pgc"
 int i = 0 ;
 
#line 135 "utext_ex.pgc"
 int j = 0 ;
 
#line 136 "utext_ex.pgc"
 int id = 0 ;
 
#line 137 "utext_ex.pgc"
 int id2 = 0 ;
 
#line 138 "utext_ex.pgc"
 utext employee_array [ 3 ] [ 41 ] ;
 
#line 139 "utext_ex.pgc"
  struct uvarchar_2  { int len; utext arr[ 101 ]; }  address_array [ 3 ] ;
/* exec sql end declare section */
#line 140 "utext_ex.pgc"


    printf("\n**************Start testcase(%d)**************\n",2);

    memset(employee_array,'a',sizeof(employee_array));
    
    for(i=0;i<3;i++)
    {
        memset((void*)&address_array[i],'a',sizeof(address_array[0]));
        address_array[i].len=0;
    }
    
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate emp_bk", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 153 "utext_ex.pgc"
   

    /* Database character set converted to Unicode */
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ename , address from emp", ECPGt_EOIT, 
	ECPGt_utext,(employee_array),(long)41,(long)3,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,(address_array),(long)101,(long)3,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 156 "utext_ex.pgc"

        
    for(i=1;i<=3;i++)
    {
        print_utext_array("employee_array",employee_array[i-1],40,i-1);
        print_utext_array("address_array",address_array[i-1].arr,address_array[i-1].len,i-1);
        /* Unicode converted to Database character */
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp_bk ( id , ename , address ) values ( $1  , $2  , $3  )", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,(employee_array[i-1]),(long)41,(long)1,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address_array[i-1]),(long)101,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 163 "utext_ex.pgc"
    
    }
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from emp_bk", ECPGt_EOIT, 
	ECPGt_int,&(total_tuples),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 166 "utext_ex.pgc"

    printf("total_tuples = %d in emp_bk\n",total_tuples);
    
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ename , address from emp", ECPGt_EOIT, 
	ECPGt_utext,(employee_array),(long)41,(long)3,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,(address_array),(long)101,(long)3,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 170 "utext_ex.pgc"

    for(i=3;i>0;i--)
    {
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select id from emp_bk where ename = $1  and address = $2 ", 
	ECPGt_utext,(employee_array[i-1]),(long)41,(long)1,(41)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address_array[i-1]),(long)101,(long)1,sizeof(struct uvarchar_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 173 "utext_ex.pgc"

        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select id from emp_bk where ename = $1  and address = $2 ", 
	ECPGt_char,(char_ename[i-1]),(long)40,(long)1,(40)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(char_addr[i-1]),(long)40,(long)1,(40)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 174 "utext_ex.pgc"

        printf("id[%d] = id2[%d] in emp_bk\n",id,id2);    
    }
}


/*
 Test utext pointer
*/
void testcase3()
{
    /* exec sql begin declare section */
       
       
       
       
             /* define Unicode host variable */
       
       /* define a variable length Unicode host variable */
    
#line 186 "utext_ex.pgc"
 int total_tuples = 0 ;
 
#line 187 "utext_ex.pgc"
 int i = 0 ;
 
#line 188 "utext_ex.pgc"
 int id = 0 ;
 
#line 189 "utext_ex.pgc"
 int id2 = 0 ;
 
#line 190 "utext_ex.pgc"
 utext * employee ;
 
#line 191 "utext_ex.pgc"
 int employee_len = 82 ;
 
#line 192 "utext_ex.pgc"
  struct uvarchar_3  { int len; utext arr[ 101 ]; }  address ;
/* exec sql end declare section */
#line 193 "utext_ex.pgc"


    
    printf("\n**************Start testcase(%d)**************\n",3);
    
    employee = malloc(employee_len);
    
    memset(employee,'a',employee_len);
    memset((void*)&address,'a',sizeof(address));
    address.len = 0;
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate emp_bk", ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 204 "utext_ex.pgc"
   
    
    for(i=1;i<=3;i++)
    {
        /* Database character set converted to Unicode */
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ename , address from emp where id = $1 ", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,&(employee),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 209 "utext_ex.pgc"

        print_ret;
        /* Unicode converted to Database character */
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp_bk ( id , ename , address ) values ( $1  , $2  , $3  )", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_utext,&(employee),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT, ECPGt_EOLT);}
#line 212 "utext_ex.pgc"
    
    }
    
    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from emp_bk", ECPGt_EOIT, 
	ECPGt_int,&(total_tuples),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 215 "utext_ex.pgc"

    printf("total_tuples = %d in emp_bk\n",total_tuples);
    
    
    for(i=3;i>0;i--)
    {
        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ename , address from emp where id = $1 ", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_utext,&(employee),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 221 "utext_ex.pgc"

        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select id from emp_bk where ename = $1  and address = $2 ", 
	ECPGt_utext,&(employee),(long)0,(long)1,(1)*sizeof(utext), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_uvarchar,&(address),(long)101,(long)1,sizeof(struct uvarchar_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 222 "utext_ex.pgc"

        { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select id from emp_bk where ename = $1  and address = $2 ", 
	ECPGt_char,(char_ename[i-1]),(long)40,(long)1,(40)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(char_addr[i-1]),(long)40,(long)1,(40)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT, ECPGt_EOLT);}
#line 223 "utext_ex.pgc"

        printf("id[%d] = id2[%d] in emp_bk\n",id,id2);    
    }
}

int main() {
    { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 229 "utext_ex.pgc"


    { ECPGsetcommit(__LINE__, "on", NULL);}
#line 231 "utext_ex.pgc"

    /* exec sql whenever sql_warning  sqlprint ; */
#line 232 "utext_ex.pgc"

    /* exec sql whenever sqlerror  sqlprint ; */
#line 233 "utext_ex.pgc"

    create_table();
    init_table();
    
    testcase1();
    testcase2();
    testcase3();

    { ECPGdisconnect(__LINE__, "CURRENT");
#line 241 "utext_ex.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 241 "utext_ex.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 241 "utext_ex.pgc"

    return 0;
}



