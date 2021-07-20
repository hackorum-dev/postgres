/*
 *	qselect.c: standard quickselect algorithm
 */

#include "c.h"

#define ST_SORT qselect
#define ST_TOP_K
#define ST_ELEMENT_TYPE_VOID
#define ST_COMPARE_RUNTIME_POINTER
#define ST_SCOPE
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

#define ST_SORT qselect_arg
#define ST_TOP_K
#define ST_ELEMENT_TYPE_VOID
#define ST_COMPARATOR_TYPE_NAME qsort_arg_comparator
#define ST_COMPARE_RUNTIME_POINTER
#define ST_COMPARE_ARG_TYPE void
#define ST_SCOPE
#define ST_DEFINE
#include "lib/sort_template.h"
