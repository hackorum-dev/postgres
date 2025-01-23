// Find cases where we can use the new pg_cmp_* functions.
//
// Copyright 2025 Mats Kindahl, Timescale.
//
// Options: --no-includes --include-headers

virtual report
virtual context
virtual patch

@initialize:python@
@@

import re

TYPMAP = {
       'BlockNumber': 'pg_cmp_u32',
       'ForkNumber': 'pg_cmp_s32',
       'OffsetNumber': 'pg_cmp_s16',
       'int': 'pg_cmp_s32',
       'int16': 'pg_cmp_s16',
       'int32': 'pg_cmp_s32',
       'uint16': 'pg_cmp_u16',
       'uint32': 'pg_cmp_u32',
       'unsigned int': 'pg_cmp_u32',
}

def is_valid(expr):
    return not re.search(r'DatumGet[A-Za-z]+', expr)

@r1e depends on context || report expression@
type TypeName : script:python() { TypeName in TYPMAP };
position pos;
TypeName lhs : script:python() { is_valid(lhs) };
TypeName rhs : script:python() { is_valid(rhs) };
@@
* lhs@pos < rhs ? -1 : lhs > rhs ? 1 : 0

@script:python depends on report@
lhs << r1e.lhs;
rhs << r1e.rhs;
pos << r1e.pos;
@@
coccilib.report.print_report(pos[0], f"conditional checks between '{lhs}' and '{rhs}' can be replaced with a PostgreSQL comparison function")

@r1 depends on context || report@
type TypeName : script:python() { TypeName in TYPMAP };
position pos;
TypeName lhs : script:python() { is_valid(lhs) };
TypeName rhs : script:python() { is_valid(rhs) };
@@
(
* if@pos (lhs < rhs) return -1; else if (lhs > rhs) return 1; return 0;
|
* if@pos (lhs < rhs) return -1; else if (lhs > rhs) return 1; else return 0;
|
* if@pos (lhs < rhs) return -1; if (lhs > rhs) return 1; return 0;
|
* if@pos (lhs > rhs) return 1; if (lhs < rhs) return -1; return 0;
|
* if@pos (lhs == rhs) return 0; if (lhs > rhs) return 1; return -1;
|
* if@pos (lhs == rhs) return 0; return lhs > rhs ? 1 : -1;
|
* if@pos (lhs == rhs) return 0; return lhs < rhs ? -1 : 1;
)

@script:python depends on report@
lhs << r1.lhs;
rhs << r1.rhs;
pos << r1.pos;
@@
coccilib.report.print_report(pos[0], f"conditional checks between '{lhs}' and '{rhs}' can be replaced with a PostgreSQL comparison function")

@expr_repl depends on patch expression@
type TypeName : script:python() { TypeName in TYPMAP };
fresh identifier cmp = script:python(TypeName) { TYPMAP[TypeName] };
TypeName lhs : script:python() { is_valid(lhs) };
TypeName rhs : script:python() { is_valid(rhs) };
@@
- lhs < rhs ? -1 : lhs > rhs ? 1 : 0
+ cmp(lhs,rhs)

@stmt_repl depends on patch@
type TypeName : script:python() { TypeName in TYPMAP };
fresh identifier cmp = script:python(TypeName) { TYPMAP[TypeName] };
TypeName lhs : script:python() { is_valid(lhs) };
TypeName rhs : script:python() { is_valid(rhs) };
@@
(
- if (lhs < rhs) return -1; if (lhs > rhs) return 1; return 0;
+ return cmp(lhs,rhs);
|
- if (lhs < rhs) return -1; else if (lhs > rhs) return 1; return 0;
+ return cmp(lhs,rhs);
|
- if (lhs < rhs) return -1; else if (lhs > rhs) return 1; else return 0;
+ return cmp(lhs,rhs);
|
- if (lhs > rhs) return 1; if (lhs < rhs) return -1; return 0;
+ return cmp(lhs,rhs);
|
- if (lhs > rhs) return 1; else if (lhs < rhs) return -1; return 0;
+ return cmp(lhs,rhs);
|
- if (lhs == rhs) return 0; if (lhs > rhs) return 1; return -1;
+ return cmp(lhs,rhs);
|
- if (lhs == rhs) return 0; return lhs > rhs ? 1 : -1;
+ return cmp(lhs,rhs);
|
- if (lhs == rhs) return 0; return lhs < rhs ? -1 : 1;
+ return cmp(lhs,rhs);
)

// Add an include if there were none and we had to do some
// replacements
@has_include depends on patch@
@@
  #include "common/int.h"

@depends on patch && !has_include && (stmt_repl || expr_repl)@
@@
  #include ...
+ #include "common/int.h"
