/*-------------------------------------------------------------------------
 *
 * bloomfilter.h
 *	  Space-efficient set membership testing
 *
 * Copyright (c) 2018-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/lib/bloomfilter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BLOOMFILTER_H
#define BLOOMFILTER_H

#include "utils/dsa.h"

typedef struct bloom_filter bloom_filter;

extern bloom_filter *bloom_create(int64 total_elems, int bloom_work_mem,
								  uint64 seed);
extern void bloom_free(bloom_filter *filter);
extern dsa_pointer bloom_create_in_dsa(dsa_area *area, int64 total_elems,
									   int bloom_work_mem, uint64 seed);
extern void bloom_free_in_dsa(dsa_area *area, dsa_pointer filter_dsa_address);
extern void bloom_add_element(bloom_filter *filter, unsigned char *elem,
							  size_t len);
extern bool bloom_lacks_element(bloom_filter *filter, unsigned char *elem,
								size_t len);
extern double bloom_prop_bits_set(bloom_filter *filter);
extern void add_to_filter(bloom_filter *main_filter, bloom_filter *to_add);
extern void replace_bitset(bloom_filter *main_filter, bloom_filter *overriding_filter);

#endif							/* BLOOMFILTER_H */
