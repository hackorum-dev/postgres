/*--------------------------------------------------------------------------
 *
 * test_lossless_bloomfilter.c
 *		Test enhanced lossless bloom filter implementation.
 *
 * This module tests the enhanced bloom filter that combines:
 * 1. Rational Bloom Filters (RBF) - allowing non-integer k values
 * 2. Variably-Sized Block Bloom Filters (VSBBF) - flexible filter sizes
 * 3. Witness Data Structure - for lossless membership testing
 * 4. Conditional Hashing - optimized hash function usage based on element properties
 *
 * Copyright (c) 2018-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_bloomfilter/test_lossless_bloomfilter.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/pg_prng.h"
#include "fmgr.h"
#include "lib/bloomfilter.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/* Fits decimal representation of PG_INT64_MIN + 2 bytes: */
#define MAX_ELEMENT_BYTES		21

/*
 * Property function for conditional hashing demo
 * Returns true if the element represents an "even" number
 */
static bool
is_even_number_property(unsigned char *elem, size_t len, void *context)
{
	char *str = (char *) elem;
	int64 num;
	
	/* Simple parsing - assume element is "i<number>" format */
	if (len > 1 && str[0] == 'i')
	{
		num = strtoll(&str[1], NULL, 10);
		return (num % 2 == 0);
	}
	
	return false;
}

/*
 * Test basic lossless bloom filter functionality
 */
PG_FUNCTION_INFO_V1(test_lossless_basic);
Datum
test_lossless_basic(PG_FUNCTION_ARGS)
{
	int64		nelements = PG_GETARG_INT64(0);
	int			bloom_work_mem = PG_GETARG_INT32(1);
	lossless_bloom_filter *filter;
	bloom_config config;
	char		element[MAX_ELEMENT_BYTES];
	int64		i;
	int64		false_positives = 0;
	int64		false_negatives = 0;
	uint64		witness_entries, total_bits, bits_set;
	int			num_blocks;
	double		fill_ratio;
	
	/* Initialize configuration */
	memset(&config, 0, sizeof(bloom_config));
	config.total_elems = nelements;
	config.bloom_work_mem = bloom_work_mem;
	config.seed = pg_prng_uint64(&pg_global_prng_state);
	config.property_func = NULL;  /* No conditional hashing for basic test */
	
	/* Create lossless bloom filter */
	filter = lossless_bloom_create(&config);
	
	/* Populate with "nelements" dummy strings */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "i" INT64_FORMAT, i);
		lossless_bloom_add_element(filter, (unsigned char *) element, strlen(element));
	}
	
	/* Test for elements that should be present (no false negatives expected) */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "i" INT64_FORMAT, i);
		if (!lossless_bloom_contains_element(filter, (unsigned char *) element, strlen(element)))
		{
			false_negatives++;
		}
	}
	
	/* Test for elements that should NOT be present (no false positives expected) */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "M" INT64_FORMAT, i);
		if (lossless_bloom_contains_element(filter, (unsigned char *) element, strlen(element)))
		{
			false_positives++;
		}
	}
	
	/* Get statistics */
	lossless_bloom_get_stats(filter, &witness_entries, &total_bits, &bits_set, &num_blocks);
	fill_ratio = lossless_bloom_prop_bits_set(filter);
	
	/* Clean up */
	lossless_bloom_free(filter);
	
	/* Return results as text */
	ereport(NOTICE,
			(errmsg("Lossless Bloom Filter Test Results:"),
			 errdetail("Elements: " INT64_FORMAT ", "
					  "False Positives: " INT64_FORMAT " (should be 0), "
					  "False Negatives: " INT64_FORMAT " (should be 0), "
					  "Witness Entries: " UINT64_FORMAT ", "
					  "Total Bits: " UINT64_FORMAT ", "
					  "Bits Set: " UINT64_FORMAT " (%.2f%%), "
					  "Blocks: %d",
					  nelements, false_positives, false_negatives,
					  witness_entries, total_bits, bits_set, fill_ratio * 100.0, num_blocks)));
	
	PG_RETURN_BOOL(false_positives == 0 && false_negatives == 0);
}

/*
 * Test conditional hashing functionality
 */
PG_FUNCTION_INFO_V1(test_lossless_conditional);
Datum
test_lossless_conditional(PG_FUNCTION_ARGS)
{
	int64		nelements = PG_GETARG_INT64(0);
	int			bloom_work_mem = PG_GETARG_INT32(1);
	lossless_bloom_filter *filter;
	bloom_config config;
	char		element[MAX_ELEMENT_BYTES];
	int64		i;
	int64		false_positives = 0;
	int64		false_negatives = 0;
	uint64		witness_entries, total_bits, bits_set;
	int			num_blocks;
	double		fill_ratio;
	
	/* Initialize configuration with conditional hashing */
	memset(&config, 0, sizeof(bloom_config));
	config.total_elems = nelements;
	config.bloom_work_mem = bloom_work_mem;
	config.seed = pg_prng_uint64(&pg_global_prng_state);
	config.property_func = is_even_number_property;
	config.property_context = NULL;
	config.property_prob_true_pos = 0.6;  /* Even numbers more likely to be true positives */
	config.property_prob_true_neg = 0.4;  /* Odd numbers less likely to be true positives */
	
	/* Create lossless bloom filter with conditional hashing */
	filter = lossless_bloom_create(&config);
	
	/* Populate with "nelements" dummy strings */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "i" INT64_FORMAT, i);
		lossless_bloom_add_element(filter, (unsigned char *) element, strlen(element));
	}
	
	/* Test for elements that should be present (no false negatives expected) */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "i" INT64_FORMAT, i);
		if (!lossless_bloom_contains_element(filter, (unsigned char *) element, strlen(element)))
		{
			false_negatives++;
		}
	}
	
	/* Test for elements that should NOT be present (no false positives expected) */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "M" INT64_FORMAT, i);
		if (lossless_bloom_contains_element(filter, (unsigned char *) element, strlen(element)))
		{
			false_positives++;
		}
	}
	
	/* Get statistics */
	lossless_bloom_get_stats(filter, &witness_entries, &total_bits, &bits_set, &num_blocks);
	fill_ratio = lossless_bloom_prop_bits_set(filter);
	
	/* Clean up */
	lossless_bloom_free(filter);
	
	/* Return results as text */
	ereport(NOTICE,
			(errmsg("Lossless Bloom Filter with Conditional Hashing Test Results:"),
			 errdetail("Elements: " INT64_FORMAT ", "
					  "False Positives: " INT64_FORMAT " (should be 0), "
					  "False Negatives: " INT64_FORMAT " (should be 0), "
					  "Witness Entries: " UINT64_FORMAT ", "
					  "Total Bits: " UINT64_FORMAT ", "
					  "Bits Set: " UINT64_FORMAT " (%.2f%%), "
					  "Blocks: %d",
					  nelements, false_positives, false_negatives,
					  witness_entries, total_bits, bits_set, fill_ratio * 100.0, num_blocks)));
	
	PG_RETURN_BOOL(false_positives == 0 && false_negatives == 0);
}

/*
 * Compare traditional vs lossless bloom filter performance
 */
PG_FUNCTION_INFO_V1(test_bloom_comparison);
Datum
test_bloom_comparison(PG_FUNCTION_ARGS)
{
	int64		nelements = PG_GETARG_INT64(0);
	int			bloom_work_mem = PG_GETARG_INT32(1);
	
	/* Traditional bloom filter test */
	bloom_filter *traditional_filter;
	uint64		traditional_seed = pg_prng_uint64(&pg_global_prng_state);
	int64		traditional_false_positives = 0;
	
	/* Lossless bloom filter test */
	lossless_bloom_filter *lossless_filter;
	bloom_config config;
	int64		lossless_false_positives = 0;
	
	char		element[MAX_ELEMENT_BYTES];
	int64		i;
	uint64		witness_entries, total_bits, bits_set;
	int			num_blocks;
	
	/* Create traditional bloom filter */
	traditional_filter = bloom_create(nelements, bloom_work_mem, traditional_seed);
	
	/* Create lossless bloom filter */
	memset(&config, 0, sizeof(bloom_config));
	config.total_elems = nelements;
	config.bloom_work_mem = bloom_work_mem;
	config.seed = traditional_seed;  /* Use same seed for fair comparison */
	config.property_func = NULL;
	lossless_filter = lossless_bloom_create(&config);
	
	/* Populate both filters with same elements */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "i" INT64_FORMAT, i);
		bloom_add_element(traditional_filter, (unsigned char *) element, strlen(element));
		lossless_bloom_add_element(lossless_filter, (unsigned char *) element, strlen(element));
	}
	
	/* Test both filters for elements that should NOT be present */
	for (i = 0; i < nelements; i++)
	{
		CHECK_FOR_INTERRUPTS();
		
		snprintf(element, sizeof(element), "M" INT64_FORMAT, i);
		
		/* Traditional filter - count false positives */
		if (!bloom_lacks_element(traditional_filter, (unsigned char *) element, strlen(element)))
		{
			traditional_false_positives++;
		}
		
		/* Lossless filter - should have zero false positives */
		if (lossless_bloom_contains_element(lossless_filter, (unsigned char *) element, strlen(element)))
		{
			lossless_false_positives++;
		}
	}
	
	/* Get lossless filter statistics */
	lossless_bloom_get_stats(lossless_filter, &witness_entries, &total_bits, &bits_set, &num_blocks);
	
	/* Clean up */
	bloom_free(traditional_filter);
	lossless_bloom_free(lossless_filter);
	
	/* Return comparison results */
	ereport(NOTICE,
			(errmsg("Bloom Filter Comparison Results:"),
			 errdetail("Elements tested: " INT64_FORMAT ", "
					  "Traditional Filter False Positives: " INT64_FORMAT " (%.2f%%), "
					  "Lossless Filter False Positives: " INT64_FORMAT " (%.2f%%), "
					  "Lossless Witness Entries: " UINT64_FORMAT ", "
					  "Lossless Blocks: %d",
					  nelements, 
					  traditional_false_positives, 
					  (double)traditional_false_positives / nelements * 100.0,
					  lossless_false_positives,
					  (double)lossless_false_positives / nelements * 100.0,
					  witness_entries, num_blocks)));
	
	PG_RETURN_BOOL(lossless_false_positives == 0);
}

/*
 * Demonstrate VSBBF block decomposition
 */
PG_FUNCTION_INFO_V1(test_vsbbf_blocks);
Datum
test_vsbbf_blocks(PG_FUNCTION_ARGS)
{
	int64		total_bits = PG_GETARG_INT64(0);
	lossless_bloom_filter *filter;
	bloom_config config;
	uint64		witness_entries, actual_total_bits, bits_set;
	int			num_blocks;
	StringInfoData result;
	
	/* Initialize configuration */
	memset(&config, 0, sizeof(bloom_config));
	config.total_elems = 1000;  /* Dummy value */
	config.bloom_work_mem = (total_bits / 8 / 1024) + 1;  /* Convert bits to KB */
	config.seed = 12345;
	config.property_func = NULL;
	
	/* Create filter to see block decomposition */
	filter = lossless_bloom_create(&config);
	
	/* Get statistics */
	lossless_bloom_get_stats(filter, &witness_entries, &actual_total_bits, &bits_set, &num_blocks);
	
	/* Build result string */
	initStringInfo(&result);
	appendStringInfo(&result, "VSBBF Block Decomposition for " INT64_FORMAT " target bits:\n", total_bits);
	appendStringInfo(&result, "Actual total bits: " UINT64_FORMAT "\n", actual_total_bits);
	appendStringInfo(&result, "Number of blocks: %d\n", num_blocks);
	
	/* Note: We can't access the internal block structure from here, 
	 * but this demonstrates the concept */
	
	/* Clean up */
	lossless_bloom_free(filter);
	
	PG_RETURN_TEXT_P(cstring_to_text(result.data));
} 