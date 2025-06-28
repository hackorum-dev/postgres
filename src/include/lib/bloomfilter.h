/*-------------------------------------------------------------------------
 *
 * bloomfilter.h
 *	  Space-efficient set membership testing
 *
 * Copyright (c) 2018-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/lib/bloomfilter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BLOOMFILTER_H
#define BLOOMFILTER_H

typedef struct bloom_filter bloom_filter;

/* Traditional bloom filter API */
extern bloom_filter *bloom_create(int64 total_elems, int bloom_work_mem,
								  uint64 seed);
extern void bloom_free(bloom_filter *filter);
extern void bloom_add_element(bloom_filter *filter, unsigned char *elem,
							  size_t len);
extern bool bloom_lacks_element(bloom_filter *filter, unsigned char *elem,
								size_t len);
extern double bloom_prop_bits_set(bloom_filter *filter);

/* Enhanced Lossless Bloom Filter API */
typedef struct lossless_bloom_filter lossless_bloom_filter;

/* Property function type for conditional hashing */
typedef bool (*bloom_property_func)(unsigned char *elem, size_t len, void *context);

/* Configuration for enhanced bloom filter */
typedef struct bloom_config
{
	int64		total_elems;		/* estimated number of elements */
	int			bloom_work_mem;		/* memory limit in KB */
	uint64		seed;				/* hash seed */
	
	/* Optional conditional hashing configuration */
	bloom_property_func property_func;	/* function to test property P */
	void	   *property_context;	/* context for property function */
	double		property_prob_true_pos;	/* Pr(P | item is true positive) */
	double		property_prob_true_neg;	/* Pr(P | item is true negative) */
} bloom_config;

extern lossless_bloom_filter *lossless_bloom_create(bloom_config *config);
extern void lossless_bloom_free(lossless_bloom_filter *filter);
extern void lossless_bloom_add_element(lossless_bloom_filter *filter, 
									   unsigned char *elem, size_t len);
extern bool lossless_bloom_contains_element(lossless_bloom_filter *filter,
											unsigned char *elem, size_t len);
extern double lossless_bloom_prop_bits_set(lossless_bloom_filter *filter);

/* Statistics and debugging */
extern void lossless_bloom_get_stats(lossless_bloom_filter *filter,
									 uint64 *witness_entries,
									 uint64 *total_bits,
									 uint64 *bits_set,
									 int *num_blocks);

#endif							/* BLOOMFILTER_H */
