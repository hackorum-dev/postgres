// Since PG16 there are array versions of common palloc operations, so
// we can use those instead.
//
// We ignore cases where we have a anonymous struct and also when the
// type of the variable being assigned to is different from the
// inferred type.
//
// Options: --no-includes --include-headers

virtual patch
virtual report
virtual context

// These rules (soN) are needed to rewrite types of the form
// sizeof(T[C]) to C * sizeof(T) since Cocci cannot (currently) handle
// it.
@initialize:python@
@@
import re

CRE = re.compile(r'(.*)\s+\[\s+(\d+)\s+\]$')

def is_array_type(s):
    mre = CRE.match(s)
    return (mre is not None)

@so1 depends on patch@
type T : script:python() {  is_array_type(T) };
@@
palloc(sizeof(T))

@script:python so2 depends on patch@
T << so1.T;
T2;
E;
@@
mre = CRE.match(T)
coccinelle.T2 = cocci.make_type(mre.group(1))
coccinelle.E = cocci.make_expr(mre.group(2))

@depends on patch@
type so1.T;
type so2.T2;
expression so2.E;
@@
- palloc(sizeof(T))
+ palloc(E * sizeof(T2))

@r1 depends on report || context@
type T !~ "^struct {";
expression E;
position p;
idexpression T *I;
identifier alloc = {palloc0, palloc};
@@
* I = alloc@p(E * sizeof(T))

@script:python depends on report@
p << r1.p;
alloc << r1.alloc;
@@
coccilib.report.print_report(p[0], f"this {alloc} can be replaced with {alloc}_array")

@depends on patch@
type T !~ "^struct {";
expression E;
T *P;
idexpression T* I;
constant C;
identifier alloc = {palloc0, palloc};
fresh identifier alloc_array = alloc ## "_array";
@@
(
- I = (T*) alloc(E * sizeof( \( *P \| P[C] \) ))
+ I = alloc_array(T, E)
|
- I = (T*) alloc(E * sizeof(T))
+ I = alloc_array(T, E)
|
- I = alloc(E * sizeof( \( *P \| P[C] \) ))
+ I = alloc_array(T, E)
|
- I = alloc(E * sizeof(T))
+ I = alloc_array(T, E)
)

@r3 depends on report || context@
type T !~ "^struct {";
expression E;
idexpression T *P;
idexpression T *I;
position p;
@@
* I = repalloc@p(P, E * sizeof(T))

@script:python depends on report@
p << r3.p;
@@
coccilib.report.print_report(p[0], "this repalloc can be replaced with repalloc_array")

@depends on patch@
type T !~ "^struct {";
expression E;
idexpression T *P1;
idexpression T *P2;
idexpression T *I;
constant C;
@@
(
- I = (T*) repalloc(P1, E * sizeof( \( *P2 \| P2[C] \) ))
+ I = repalloc_array(P1, T, E)
|
- I = (T*) repalloc(P1, E * sizeof(T))
+ I = repalloc_array(P1, T, E)
|
- I = repalloc(P1, E * sizeof( \( *P2 \| P2[C] \) ))
+ I = repalloc_array(P1, T, E)
|
- I = repalloc(P1, E * sizeof(T))
+ I = repalloc_array(P1, T, E)
)

@r4 depends on report || context@
type T !~ "^struct {";
position p;
idexpression T* I;
identifier alloc = {palloc, palloc0};
@@
* I = alloc@p(sizeof(T))

@script:python depends on report@
p << r4.p;
alloc << r4.alloc;
@@
coccilib.report.print_report(p[0], f"this {alloc} can be replaced with {alloc}_object")

@depends on patch@
type T !~ "^struct {";
T* P;
idexpression T *I;
constant C;
identifier alloc = {palloc, palloc0};
fresh identifier alloc_object = alloc ## "_object";
@@
(
- I = (T*) alloc(sizeof( \( *P \| P[C] \) ))
+ I = alloc_object(T)
|
- I = (T*) alloc(sizeof(T))
+ I = alloc_object(T)
|
- I = alloc(sizeof( \( *P \| P[C] \) ))
+ I = alloc_object(T)
|
- I = alloc(sizeof(T))
+ I = alloc_object(T)
)
