virtual report
virtual context
virtual patch

@initialize:python@
@@
import re

CONST_CRE = re.compile(r'\bconst\b')

def is_simple_type(s):
    return s != 'void' and not CONST_CRE.search(s)

@r1 depends on report || context@
type T1 : script:python () { is_simple_type(T1) };
idexpression T1 *I;
type T2 != T1;
position p;
expression E;
identifier func = {palloc, palloc0};
@@
(
* I = func@p(sizeof(T2))
|
* I = func@p(E * sizeof(T2))
)

@script:python depends on report@
T1 << r1.T1;
T2 << r1.T2;
I << r1.I;
p << r1.p;
@@
coccilib.report.print_report(p[0], f"'{I}' has type '{T1}*' but 'sizeof({T2})' is used to allocate memory")

@depends on patch@
type T1 : script:python () { is_simple_type(T1) };
idexpression T1 *I;
type T2 != T1;
expression E;
identifier func = {palloc, palloc0};
@@
(
- I = func(sizeof(T2))
+ I = func(sizeof(T1))
|
- I = func(E * sizeof(T2))
+ I = func(E * sizeof(T1))
)
