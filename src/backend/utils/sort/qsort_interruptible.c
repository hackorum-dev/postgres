/*
 *	qsort_interruptible.c: qsort_arg that includes CHECK_FOR_INTERRUPTS
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/qsort_interruptible.c
 *
 */

#include "postgres.h"
#include "miscadmin.h"

#define ST_SORT qsort_interruptible
#define ST_ELEMENT_TYPE_VOID
#define ST_COMPARATOR_TYPE_NAME qsort_arg_comparator
#define ST_COMPARE_RUNTIME_POINTER
#define ST_COMPARE_ARG_TYPE void
#define ST_SCOPE
#define ST_DEFINE
#define ST_CHECK_FOR_INTERRUPTS
#include "lib/sort_template.h"
