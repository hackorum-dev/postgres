// Replace uses of StringInfo with StringInfoData where the info is
// dynamically allocated but (optionally) freed at the end of the
// block. This will avoid one dynamic allocation that otherwise have
// to be dealt with.
//
// For example, this code:
//
//    StringInfo info = makeStringInfo();
//    ...
//    appendStringInfo(info, ...);
//    ...
//    return do_stuff(..., info->data, ...);
//
// Can be replaced with:
//
//    StringInfoData info;
//    initStringInfo(&info);
//    ...
//    appendStringInfo(&info, ...);
//    ...
//    return do_stuff(..., info.data, ...);

virtual report
virtual context
virtual patch

// This rule captures the position of the makeStringInfo() and bases
// all changes around that. It matches the case that we should *not*
// replace, that is, those that either (1) return the pointer or (2)
// assign the pointer to a variable or (3) assign a variable to the
// pointer.
//
// The first two cases are matched because they could potentially leak
// the pointer outside the function, for some expressions, but the
// last one is just a convenience.
//
// If we replace this, the resulting change will result in a value
// copy of a structure, which might not be optimal, so we do not do a
// replacement.
@id1 exists@
typedef StringInfo;
local idexpression StringInfo info;
position pos;
expression E;
@@
  info@pos = makeStringInfo()
  ...
(
  return info;
|
  info = E
|
  E = info
)

@r1 depends on !patch disable decl_init exists@
identifier info, fld;
position dpos, pos != id1.pos;
@@
(
* StringInfo@dpos info;
  ...
* info@pos = makeStringInfo();
|
* StringInfo@dpos info@pos = makeStringInfo();
)
<...
(
* \(pfree\|destroyStringInfo\)(info);
|
* info->fld
|
* *info
|
* info
)
...>

@script:python depends on report@
info << r1.info;
dpos << r1.dpos;
@@
coccilib.report.print_report(dpos[0], f"Variable '{info}' of type StringInfo can be defined using StringInfoData")

@depends on patch disable decl_init exists@
identifier info, fld;
position pos != id1.pos;
@@
- StringInfo info;
+ StringInfoData info;
  ...
- info@pos = makeStringInfo();
+ initStringInfo(&info);
<...
(
- \(destroyStringInfo\|pfree\)(info);
|
  info
- ->fld
+ .fld
|
- *info
+ info
|
- info
+ &info
)
...>

// Here we repeat the matching of the "bad case" since we cannot
// inherit over modifications
@id2 exists@
typedef StringInfo;
local idexpression StringInfo info;
position pos;
expression E;
@@
  info@pos = makeStringInfo()
  ...
(
  return info;
|
  info = E
|
  E = info
)

@depends on patch exists@
identifier info, fld;
position pos != id2.pos;
statement S, S1;
@@
- StringInfo info@pos = makeStringInfo();
+ StringInfoData info;
  ... when != S
(
<...
(
- \(destroyStringInfo\|pfree\)(info);
|
  info
- ->fld
+ .fld
|
- *info
+ info
|
- info
+ &info
)
...>
&
+ initStringInfo(&info);
  S1
)
