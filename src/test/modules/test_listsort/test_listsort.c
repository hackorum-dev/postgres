/*-------------------------------------------------------------------------
 *
 * test_listsort.c
 *	  Simple example of list_sort implementation using x86-simd-sort library
 *
 * Copyright (c) 2007-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_listsort/test_listsort.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <signal.h>

#include "funcapi.h"
#include "utils/array.h"

PG_MODULE_MAGIC;

typedef struct SortTestData
{
	float		value;
	int			tie_breaker;
} SortTestData;

static int
tie_breaker(const ListCell *a, const ListCell *b)
{
	SortTestData *sort_test_data = (SortTestData *) a->ptr_value;
	int			val1 = sort_test_data->tie_breaker;

	SortTestData *sort_test_data2 = (SortTestData *) b->ptr_value;
	int			val2 = sort_test_data2->tie_breaker;

	if (val1 > val2)
	{
		return 1;
	}
	else if (val1 < val2)
	{
		return -1;
	}
	else
	{
		return 0;
	}
	return 1;
}

static int
comparator(const ListCell *a, const ListCell *b)
{
	SortTestData *sort_test_data = (SortTestData *) a->ptr_value;
	float		val1 = sort_test_data->value;

	SortTestData *sort_test_data2 = (SortTestData *) b->ptr_value;
	float		val2 = sort_test_data2->value;

	if (val1 > val2)
	{
		return 1;
	}
	else if (val1 < val2)
	{
		return -1;
	}
	else
	{
		return tie_breaker(a, b);
	}
}

static float
get_value(const ListCell *a)
{
	SortTestData *sort_test_data = (SortTestData *) a->ptr_value;

	return sort_test_data->value;
}

/*  Creates ListCells with each ListCell having a float value.
 *  The max_random and tie_case_limit parameters are used to introduce ties.
 */
static ListCell **
create_list_cell(ListCell *list_cells, int size, int max_random, int tie_case_limit)
{
	ListCell  **list_cell_ptrs = (ListCell **) palloc(size * sizeof(ListCell *));

	if (list_cell_ptrs == NULL)
	{
		elog(ERROR, "error allocating memory");
	}
	srand(42);

	for (int i = 0; i < size; i++)
	{
		ListCell	current_cell;
		ListCell   *cell = (ListCell *) palloc(sizeof(ListCell));
		SortTestData *sort_test_data;
		float		random_float;
		int			random_number;
		int			bounded_random_number;

		cell = (ListCell *) palloc(sizeof(ListCell));
		if (cell == NULL)
		{
			elog(ERROR, "error allocating memory");
		}

		sort_test_data = (SortTestData *) palloc(sizeof(SortTestData));
		if (sort_test_data == NULL)
		{
			elog(ERROR, "error allocating memory");
		}

		random_float = 0;
		random_number = rand();
		bounded_random_number = random_number % max_random;
		if (bounded_random_number > 0 && bounded_random_number <= tie_case_limit)
		{
			random_float = bounded_random_number;
		}
		else
		{
			random_float = (float) random_number / (float) RAND_MAX;
		}

		sort_test_data->value = random_float;
		sort_test_data->tie_breaker = i;
		cell->ptr_value = sort_test_data;
		current_cell = *cell;
		list_cells[i] = current_cell;
		list_cell_ptrs[i] = cell;
	}

	return list_cell_ptrs;
}

Datum		test_listsort(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(test_listsort);
Datum
test_listsort(PG_FUNCTION_ARGS)
{
	int			size = PG_GETARG_INT32(0);
	int			random_number = PG_GETARG_INT32(1);
	int			tie_breaker_limit = PG_GETARG_INT32(2);
	bool		call_simd_sort = PG_GETARG_BOOL(3);

	ListCell   *list_cells;
	ListCell  **list_cell_ptrs;
	List	   *list;
	Datum	   *array_elements;
	ArrayType  *result;

	list_cells = (ListCell *) palloc(size * sizeof(ListCell));
	if (list_cells == NULL)
	{
		elog(ERROR, "error allocating memory");
	}

	list_cell_ptrs = create_list_cell(list_cells, size, random_number, tie_breaker_limit);
	if (list_cell_ptrs == NULL)
	{
		elog(ERROR, "error allocating memory");
	}

	list = (List *) palloc(sizeof(List));
	if (list == NULL)
	{
		elog(ERROR, "error allocating memory");
	}

	list->elements = list_cells;
	list->length = size;
	list->max_length = size;
	list->type = T_List;

	if (call_simd_sort)
	{
		list_sort_simd_float(list, get_value, comparator);
	}
	else
	{
		list_sort(list, comparator);
	}

	array_elements = (Datum *) palloc(sizeof(Datum) * 10);
	for (int i = 0; i < 10; i++)
	{
		array_elements[i] = Float4GetDatum(get_value(&list_cells[i]));
	}

	result = construct_array_builtin(array_elements, 10, FLOAT4OID);

	pfree(array_elements);
	pfree(list_cells);
	pfree(list);
	for (int i = 0; i < size; i++)
	{
		pfree(list_cell_ptrs[i]->ptr_value);
		pfree(list_cell_ptrs[i]);
	}
	pfree(list_cell_ptrs);

	PG_RETURN_ARRAYTYPE_P(result);
}
