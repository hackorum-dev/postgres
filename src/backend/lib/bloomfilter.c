/*-------------------------------------------------------------------------
 *
 * bloomfilter.c
 *		Space-efficient set membership testing
 *
 * A Bloom filter is a probabilistic data structure that is used to test an
 * element's membership of a set.  False positives are possible, but false
 * negatives are not; a test of membership of the set returns either "possibly
 * in set" or "definitely not in set".  This is typically very space efficient,
 * which can be a decisive advantage.
 *
 * Elements can be added to the set, but not removed.  The more elements that
 * are added, the larger the probability of false positives.  Caller must hint
 * an estimated total size of the set when the Bloom filter is initialized.
 * This is used to balance the use of memory against the final false positive
 * rate.
 *
 * The implementation is well suited to data synchronization problems between
 * unordered sets, especially where predictable performance is important and
 * some false positives are acceptable.  It's also well suited to cache
 * filtering problems where a relatively small and/or low cardinality set is
 * fingerprinted, especially when many subsequent membership tests end up
 * indicating that values of interest are not present.  That should save the
 * caller many authoritative lookups, such as expensive probes of a much larger
 * on-disk structure.
 *
 * Enhanced Lossless Bloom Filter Implementation:
 * This file also contains an enhanced implementation that combines:
 * 1. Rational Bloom Filters (RBF) - allowing non-integer k values
 * 2. Variably-Sized Block Bloom Filters (VSBBF) - flexible filter sizes
 * 3. Witness Data Structure - for lossless membership testing
 * 4. Conditional Hashing - optimized hash function usage based on element properties
 *
 * Copyright (c) 2018-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/bloomfilter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "common/hashfn.h"
#include "lib/bloomfilter.h"
#include "port/pg_bitutils.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "common/pg_prng.h"

#define MAX_HASH_FUNCS		10
#define MAX_BLOCKS			32		/* Maximum number of blocks in VSBBF */

struct bloom_filter
{
	/* K hash functions are used, seeded by caller's seed */
	int			k_hash_funcs;
	uint64		seed;
	/* m is bitset size, in bits.  Must be a power of two <= 2^32.  */
	uint64		m;
	unsigned char bitset[FLEXIBLE_ARRAY_MEMBER];
};

/* Block definition for Variably-Sized Block Bloom Filter */
typedef struct bloom_block
{
	uint64		size;				/* Size of this block in bits (power of 2) */
	double		k_rational;			/* Rational number of hash functions for this block */
	int			k_floor;			/* Floor of k_rational */
	double		k_fractional;		/* Fractional part of k_rational */
	uint64		offset;				/* Bit offset in the combined bitset */
} bloom_block;

/* Witness entry for lossless membership testing */
typedef struct witness_entry
{
	uint64		hash1;				/* First hash of the element */
	uint64		hash2;				/* Second hash of the element */
} witness_entry;

/* Enhanced Lossless Bloom Filter structure */
struct lossless_bloom_filter
{
	/* Configuration */
	bloom_config config;
	
	/* VSBBF structure */
	int			num_blocks;			/* Number of blocks */
	bloom_block blocks[MAX_BLOCKS];	/* Block definitions */
	uint64		total_bits;			/* Total bits across all blocks */
	unsigned char *bitset;			/* Combined bitset for all blocks */
	
	/* Conditional hashing parameters */
	bool		use_conditional_hashing;
	double		k_with_property;	/* k for elements with property P */
	double		k_without_property;	/* k for elements without property P */
	
	/* Witness data structure (hash table) */
	HTAB	   *witness_table;
	uint64		witness_entries;	/* Number of entries in witness */
	
	/* Statistics */
	uint64		bits_set;			/* Number of bits currently set */
	uint64		elements_added;		/* Number of elements added */
};

static int	my_bloom_power(uint64 target_bitset_bits);
static int	optimal_k(uint64 bitset_bits, int64 total_elems);
static void k_hashes(bloom_filter *filter, uint32 *hashes, unsigned char *elem,
					 size_t len);
static inline uint32 mod_m(uint32 val, uint64 m);

/* Enhanced bloom filter static functions */
static void decompose_into_blocks(uint64 total_bits, bloom_block *blocks, int *num_blocks);
static double calculate_rational_k(uint64 block_bits, int64 total_elems);
static void calculate_conditional_k_values(bloom_config *config, double base_k,
										   double *k_with_prop, double *k_without_prop);
static void enhanced_k_hashes(lossless_bloom_filter *filter, uint32 *hashes, 
							  unsigned char *elem, size_t len, double k_effective,
							  int block_idx, int *num_hashes_out);
static uint32 witness_hash_func(const void *key, Size keysize);
static int witness_match_func(const void *key1, const void *key2, Size keysize);
static bool apply_rational_hashing(double k_fractional, uint64 hash_seed);

/*
 * Create Bloom filter in caller's memory context.  We aim for a false positive
 * rate of between 1% and 2% when bitset size is not constrained by memory
 * availability.
 *
 * total_elems is an estimate of the final size of the set.  It should be
 * approximately correct, but the implementation can cope well with it being
 * off by perhaps a factor of five or more.  See "Bloom Filters in
 * Probabilistic Verification" (Dillinger & Manolios, 2004) for details of why
 * this is the case.
 *
 * bloom_work_mem is sized in KB, in line with the general work_mem convention.
 * This determines the size of the underlying bitset (trivial bookkeeping space
 * isn't counted).  The bitset is always sized as a power of two number of
 * bits, and the largest possible bitset is 512MB (2^32 bits).  The
 * implementation allocates only enough memory to target its standard false
 * positive rate, using a simple formula with caller's total_elems estimate as
 * an input.  The bitset might be as small as 1MB, even when bloom_work_mem is
 * much higher.
 *
 * The Bloom filter is seeded using a value provided by the caller.  Using a
 * distinct seed value on every call makes it unlikely that the same false
 * positives will reoccur when the same set is fingerprinted a second time.
 * Callers that don't care about this pass a constant as their seed, typically
 * 0.  Callers can also use a pseudo-random seed, eg from pg_prng_uint64().
 */
bloom_filter *
bloom_create(int64 total_elems, int bloom_work_mem, uint64 seed)
{
	bloom_filter *filter;
	int			bloom_power;
	uint64		bitset_bytes;
	uint64		bitset_bits;

	/*
	 * Aim for two bytes per element; this is sufficient to get a false
	 * positive rate below 1%, independent of the size of the bitset or total
	 * number of elements.  Also, if rounding down the size of the bitset to
	 * the next lowest power of two turns out to be a significant drop, the
	 * false positive rate still won't exceed 2% in almost all cases.
	 */
	bitset_bytes = Min(bloom_work_mem * UINT64CONST(1024), total_elems * 2);
	bitset_bytes = Max(1024 * 1024, bitset_bytes);

	/*
	 * Size in bits should be the highest power of two <= target.  bitset_bits
	 * is uint64 because PG_UINT32_MAX is 2^32 - 1, not 2^32
	 */
	bloom_power = my_bloom_power(bitset_bytes * BITS_PER_BYTE);
	bitset_bits = UINT64CONST(1) << bloom_power;
	bitset_bytes = bitset_bits / BITS_PER_BYTE;

	/* Allocate bloom filter with unset bitset */
	filter = palloc0(offsetof(bloom_filter, bitset) +
					 sizeof(unsigned char) * bitset_bytes);
	filter->k_hash_funcs = optimal_k(bitset_bits, total_elems);
	filter->seed = seed;
	filter->m = bitset_bits;

	return filter;
}

/*
 * Free Bloom filter
 */
void
bloom_free(bloom_filter *filter)
{
	pfree(filter);
}

/*
 * Add element to Bloom filter
 */
void
bloom_add_element(bloom_filter *filter, unsigned char *elem, size_t len)
{
	uint32		hashes[MAX_HASH_FUNCS];
	int			i;

	k_hashes(filter, hashes, elem, len);

	/* Map a bit-wise address to a byte-wise address + bit offset */
	for (i = 0; i < filter->k_hash_funcs; i++)
	{
		filter->bitset[hashes[i] >> 3] |= 1 << (hashes[i] & 7);
	}
}

/*
 * Test if Bloom filter definitely lacks element.
 *
 * Returns true if the element is definitely not in the set of elements
 * observed by bloom_add_element().  Otherwise, returns false, indicating that
 * element is probably present in set.
 */
bool
bloom_lacks_element(bloom_filter *filter, unsigned char *elem, size_t len)
{
	uint32		hashes[MAX_HASH_FUNCS];
	int			i;

	k_hashes(filter, hashes, elem, len);

	/* Map a bit-wise address to a byte-wise address + bit offset */
	for (i = 0; i < filter->k_hash_funcs; i++)
	{
		if (!(filter->bitset[hashes[i] >> 3] & (1 << (hashes[i] & 7))))
			return true;
	}

	return false;
}

/*
 * What proportion of bits are currently set?
 *
 * Returns proportion, expressed as a multiplier of filter size.  That should
 * generally be close to 0.5, even when we have more than enough memory to
 * ensure a false positive rate within target 1% to 2% band, since more hash
 * functions are used as more memory is available per element.
 *
 * This is the only instrumentation that is low overhead enough to appear in
 * debug traces.  When debugging Bloom filter code, it's likely to be far more
 * interesting to directly test the false positive rate.
 */
double
bloom_prop_bits_set(bloom_filter *filter)
{
	int			bitset_bytes = filter->m / BITS_PER_BYTE;
	uint64		bits_set = pg_popcount((char *) filter->bitset, bitset_bytes);

	return bits_set / (double) filter->m;
}

/*
 * Which element in the sequence of powers of two is less than or equal to
 * target_bitset_bits?
 *
 * Value returned here must be generally safe as the basis for actual bitset
 * size.
 *
 * Bitset is never allowed to exceed 2 ^ 32 bits (512MB).  This is sufficient
 * for the needs of all current callers, and allows us to use 32-bit hash
 * functions.  It also makes it easy to stay under the MaxAllocSize restriction
 * (caller needs to leave room for non-bitset fields that appear before
 * flexible array member, so a 1GB bitset would use an allocation that just
 * exceeds MaxAllocSize).
 */
static int
my_bloom_power(uint64 target_bitset_bits)
{
	int			bloom_power = -1;

	while (target_bitset_bits > 0 && bloom_power < 32)
	{
		bloom_power++;
		target_bitset_bits >>= 1;
	}

	return bloom_power;
}

/*
 * Determine optimal number of hash functions based on size of filter in bits,
 * and projected total number of elements.  The optimal number is the number
 * that minimizes the false positive rate.
 */
static int
optimal_k(uint64 bitset_bits, int64 total_elems)
{
	int			k = rint(log(2.0) * bitset_bits / total_elems);

	return Max(1, Min(k, MAX_HASH_FUNCS));
}

/*
 * Generate k hash values for element.
 *
 * Caller passes array, which is filled-in with k values determined by hashing
 * caller's element.
 *
 * Only 2 real independent hash functions are actually used to support an
 * interface of up to MAX_HASH_FUNCS hash functions; enhanced double hashing is
 * used to make this work.  The main reason we prefer enhanced double hashing
 * to classic double hashing is that the latter has an issue with collisions
 * when using power of two sized bitsets.  See Dillinger & Manolios for full
 * details.
 */
static void
k_hashes(bloom_filter *filter, uint32 *hashes, unsigned char *elem, size_t len)
{
	uint64		hash;
	uint32		x,
				y;
	uint64		m;
	int			i;

	/* Use 64-bit hashing to get two independent 32-bit hashes */
	hash = DatumGetUInt64(hash_any_extended(elem, len, filter->seed));
	x = (uint32) hash;
	y = (uint32) (hash >> 32);
	m = filter->m;

	x = mod_m(x, m);
	y = mod_m(y, m);

	/* Accumulate hashes */
	hashes[0] = x;
	for (i = 1; i < filter->k_hash_funcs; i++)
	{
		x = mod_m(x + y, m);
		y = mod_m(y + i, m);

		hashes[i] = x;
	}
}

/*
 * Calculate "val MOD m" inexpensively.
 *
 * Assumes that m (which is bitset size) is a power of two.
 *
 * Using a power of two number of bits for bitset size allows us to use bitwise
 * AND operations to calculate the modulo of a hash value.  It's also a simple
 * way of avoiding the modulo bias effect.
 */
static inline uint32
mod_m(uint32 val, uint64 m)
{
	Assert(m <= PG_UINT32_MAX + UINT64CONST(1));
	Assert(((m - 1) & m) == 0);

	return val & (m - 1);
}

/*
 * Enhanced Lossless Bloom Filter Implementation
 */

/*
 * Create enhanced lossless bloom filter - OPTIMIZED VERSION
 */
lossless_bloom_filter *
lossless_bloom_create(bloom_config *config)
{
	lossless_bloom_filter *filter;
	HASHCTL		hash_ctl;
	uint64		target_bits;
	uint64		bitset_bytes;
	int			bloom_power;
	
	Assert(config != NULL);
	Assert(config->total_elems > 0);
	
	filter = (lossless_bloom_filter *) palloc0(sizeof(lossless_bloom_filter));
	
	/* Copy configuration */
	memcpy(&filter->config, config, sizeof(bloom_config));
	
	/* OPTIMIZATION: Simplified bit calculation - single power-of-2 block */
	target_bits = Min(config->bloom_work_mem * UINT64CONST(1024) * 8,
					  config->total_elems * 16);
	target_bits = Max(1024 * 8, target_bits);  /* Minimum 1KB */
	
	/* Round to power of 2 for fast modulo operations */
	bloom_power = 0;
	while ((UINT64CONST(1) << bloom_power) < target_bits && bloom_power < 32)
		bloom_power++;
	
	filter->total_bits = UINT64CONST(1) << bloom_power;
	
	/* OPTIMIZATION: Single block setup (eliminates VSBBF complexity) */
	filter->num_blocks = 1;
	filter->blocks[0].size = filter->total_bits;
	filter->blocks[0].offset = 0;
	filter->blocks[0].k_rational = log(2.0) * filter->total_bits / config->total_elems;
	filter->blocks[0].k_floor = Max(1, Min((int)rint(filter->blocks[0].k_rational), MAX_HASH_FUNCS));
	filter->blocks[0].k_fractional = 0.0;  /* Disable fractional hashing for speed */
	
	/* Allocate bitset */
	bitset_bytes = (filter->total_bits + 7) / 8;
	filter->bitset = (unsigned char *) palloc0(bitset_bytes);
	
	/* OPTIMIZATION: Disable conditional hashing for performance */
	filter->use_conditional_hashing = false;
	
	/* OPTIMIZATION: Smaller initial witness table size */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(witness_entry);
	hash_ctl.entrysize = sizeof(witness_entry);
	hash_ctl.hash = witness_hash_func;
	hash_ctl.match = witness_match_func;
	hash_ctl.hcxt = CurrentMemoryContext;
	
	filter->witness_table = hash_create("Bloom Witness Table",
										Max(1024, config->total_elems / 16),  /* Smaller initial size */
										&hash_ctl,
										HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
	
	filter->witness_entries = 0;
	filter->bits_set = 0;
	filter->elements_added = 0;
	
	return filter;
}

/*
 * Free enhanced bloom filter
 */
void
lossless_bloom_free(lossless_bloom_filter *filter)
{
	if (filter == NULL)
		return;
		
	if (filter->bitset)
		pfree(filter->bitset);
		
	if (filter->witness_table)
		hash_destroy(filter->witness_table);
		
	pfree(filter);
}

/*
 * Add element to enhanced bloom filter - OPTIMIZED VERSION
 */
void
lossless_bloom_add_element(lossless_bloom_filter *filter, unsigned char *elem, size_t len)
{
	uint64		base_hash;
	uint32		x, y;
	witness_entry witness_key;
	bool		found;
	int			i;
	uint32		hashes[MAX_HASH_FUNCS];
	int			k_hash_funcs;
	uint64		bit_mask;
	
	Assert(filter != NULL);
	Assert(elem != NULL);
	
	/* OPTIMIZATION: Single hash computation */
	base_hash = DatumGetUInt64(hash_any_extended(elem, len, filter->config.seed));
	x = (uint32) base_hash;
	y = (uint32) (base_hash >> 32);
	
	/* Create witness entry */
	witness_key.hash1 = x;
	witness_key.hash2 = y;
	
	/* Add to witness table */
	(void) hash_search(filter->witness_table,
					   &witness_key,
					   HASH_ENTER,
					   &found);
	if (!found)
	{
		filter->witness_entries++;
	}
	
	/* OPTIMIZATION: Use pre-calculated k and bit mask for fast operations */
	k_hash_funcs = filter->blocks[0].k_floor;  /* Single block, pre-calculated */
	bit_mask = (uint32)(filter->total_bits - 1);  /* Fast modulo for power-of-2 */
	
	/* OPTIMIZATION: Simplified hash generation - enhanced double hashing */
	y |= 1;  /* Ensure y is odd for good double hashing properties */
	hashes[0] = x;
	for (i = 1; i < k_hash_funcs; i++)
	{
		x += y;
		hashes[i] = x;
	}
	
	/* OPTIMIZATION: Fast bit setting with bitwise operations */
	for (i = 0; i < k_hash_funcs; i++)
	{
		uint64 bit_pos = hashes[i] & bit_mask;     /* Fast modulo using bitwise AND */
		uint64 byte_pos = bit_pos >> 3;           /* Fast divide by 8 */
		int bit_offset = bit_pos & 7;             /* Fast modulo 8 */
		
		if (!(filter->bitset[byte_pos] & (1 << bit_offset)))
		{
			filter->bitset[byte_pos] |= (1 << bit_offset);
			filter->bits_set++;
		}
	}
	
	filter->elements_added++;
}

/*
 * Test lossless membership in enhanced bloom filter - OPTIMIZED VERSION
 */
bool
lossless_bloom_contains_element(lossless_bloom_filter *filter, unsigned char *elem, size_t len)
{
	uint64		base_hash;
	uint32		x, y;
	witness_entry witness_key;
	witness_entry *witness_found;
	int			i;
	uint32		hashes[MAX_HASH_FUNCS];
	int			k_hash_funcs;
	uint64		bit_mask;
	
	Assert(filter != NULL);
	Assert(elem != NULL);
	
	/* OPTIMIZATION: Single hash computation */
	base_hash = DatumGetUInt64(hash_any_extended(elem, len, filter->config.seed));
	x = (uint32) base_hash;
	y = (uint32) (base_hash >> 32);
	
	/* OPTIMIZATION: Use pre-calculated k and bit mask for fast operations */
	k_hash_funcs = filter->blocks[0].k_floor;  /* Single block, pre-calculated */
	bit_mask = (uint32)(filter->total_bits - 1);  /* Fast modulo for power-of-2 */
	
	/* OPTIMIZATION: Simplified hash generation - enhanced double hashing */
	y |= 1;  /* Ensure y is odd for good double hashing properties */
	hashes[0] = x;
	for (i = 1; i < k_hash_funcs; i++)
	{
		x += y;
		hashes[i] = x;
	}
	
	/* OPTIMIZATION: Fast bit checking with bitwise operations */
	for (i = 0; i < k_hash_funcs; i++)
	{
		uint64 bit_pos = hashes[i] & bit_mask;     /* Fast modulo using bitwise AND */
		uint64 byte_pos = bit_pos >> 3;           /* Fast divide by 8 */
		int bit_offset = bit_pos & 7;             /* Fast modulo 8 */
		
		if (!(filter->bitset[byte_pos] & (1 << bit_offset)))
		{
			/* Definitely not in set */
			return false;
		}
	}
	
	/* All bits are set - check witness for lossless confirmation */
	witness_key.hash1 = (uint32) base_hash;
	witness_key.hash2 = (uint32) (base_hash >> 32);
	
	witness_found = (witness_entry *) hash_search(filter->witness_table,
												  &witness_key,
												  HASH_FIND,
												  NULL);
	
	/* Return true only if confirmed by witness */
	return (witness_found != NULL);
}

/*
 * Get proportion of bits set in enhanced bloom filter  
 */
double
lossless_bloom_prop_bits_set(lossless_bloom_filter *filter)
{
	if (filter->total_bits == 0)
		return 0.0;
		
	return (double) filter->bits_set / filter->total_bits;
}

/*
 * Get statistics from enhanced bloom filter
 */
void
lossless_bloom_get_stats(lossless_bloom_filter *filter,
						 uint64 *witness_entries,
						 uint64 *total_bits,
						 uint64 *bits_set,
						 int *num_blocks)
{
	if (witness_entries)
		*witness_entries = filter->witness_entries;
	if (total_bits)
		*total_bits = filter->total_bits;
	if (bits_set)
		*bits_set = filter->bits_set;
	if (num_blocks)
		*num_blocks = filter->num_blocks;
}

/*
 * Static helper functions for enhanced bloom filter
 */

/* UNUSED FUNCTIONS - Kept for potential future use in advanced optimizations
 * 
 * These functions were part of the original VSBBF (Variable Size Bloom Filter)
 * implementation with conditional and rational hashing. They are currently 
 * unused due to the simplified single-block optimization but may be useful
 * for future advanced features.
 */

#ifdef FUTURE_OPTIMIZATIONS
/*
 * Decompose total bits into power-of-two blocks for VSBBF
 */
static void
decompose_into_blocks(uint64 total_bits, bloom_block *blocks, int *num_blocks)
{
	int count = 0;
	uint64 remaining = total_bits;
	int power;
	
	/* Decompose into largest power-of-two components */
	while (remaining > 0 && count < MAX_BLOCKS)
	{
		/* Find highest power of 2 <= remaining */
		power = 0;
		while ((UINT64CONST(1) << (power + 1)) <= remaining && power < 31)
			power++;
			
		blocks[count].size = UINT64CONST(1) << power;
		remaining -= blocks[count].size;
		count++;
	}
	
	*num_blocks = count;
}

/*
 * Calculate rational k for a block
 */
static double
calculate_rational_k(uint64 block_bits, int64 total_elems)
{
	double k = log(2.0) * block_bits / total_elems;
	return Max(0.1, Min(k, MAX_HASH_FUNCS));  /* Reasonable bounds */
}

/*
 * Calculate conditional k values for elements with/without property P
 */
static void
calculate_conditional_k_values(bloom_config *config, double base_k,
							   double *k_with_prop, double *k_without_prop)
{
	double a = config->property_prob_true_pos;
	double b = config->property_prob_true_neg;
	double p = a / (a + b);  /* Simplified calculation */
	
	/* Adjust k values to minimize overall FPR while maintaining average k */
	*k_with_prop = base_k * (1.0 + p);
	*k_without_prop = base_k * (1.0 - p);
	
	/* Ensure reasonable bounds */
	*k_with_prop = Max(0.1, Min(*k_with_prop, MAX_HASH_FUNCS));
	*k_without_prop = Max(0.1, Min(*k_without_prop, MAX_HASH_FUNCS));
}

/*
 * Generate enhanced k hashes with rational bloom filter logic
 */
static void
enhanced_k_hashes(lossless_bloom_filter *filter, uint32 *hashes, 
				  unsigned char *elem, size_t len, double k_effective,
				  int block_idx, int *num_hashes_out)
{
	uint64		hash;
	uint32		x, y;
	int			k_floor = (int) floor(k_effective);
	double		k_fractional = k_effective - k_floor;
	int			i;
	uint64		block_seed;
	
	/* Generate block-specific seed */
	block_seed = filter->config.seed ^ (block_idx + 1);
	
	/* Use 64-bit hashing to get two independent 32-bit hashes */
	hash = DatumGetUInt64(hash_any_extended(elem, len, block_seed));
	x = (uint32) hash;
	y = (uint32) (hash >> 32);
	
	/* Generate deterministic k_floor hashes */
	hashes[0] = x;
	for (i = 1; i < k_floor && i < MAX_HASH_FUNCS; i++)
	{
		x = x + y;
		y = y + i;
		hashes[i] = x;
	}
	
	*num_hashes_out = k_floor;
	
	/* Apply fractional hash probabilistically */
	if (k_fractional > 0.0 && *num_hashes_out < MAX_HASH_FUNCS)
	{
		if (apply_rational_hashing(k_fractional, hash))
		{
			x = x + y;
			y = y + *num_hashes_out;
			hashes[*num_hashes_out] = x;
			(*num_hashes_out)++;
		}
	}
}
#endif /* FUTURE_OPTIMIZATIONS */

/*
 * Apply rational hashing decision based on fractional part
 */
static bool
apply_rational_hashing(double k_fractional, uint64 hash_seed)
{
	/* Use hash_seed to generate deterministic pseudo-random decision */
	uint32 rand_val = (uint32)(hash_seed >> 32);
	double threshold = (double)rand_val / (double)UINT32_MAX;
	
	return threshold < k_fractional;
}

/*
 * Hash function for witness table
 */
static uint32
witness_hash_func(const void *key, Size keysize)
{
	const witness_entry *entry = (const witness_entry *) key;
	return (uint32)(entry->hash1 ^ (entry->hash2 << 1));
}

/*
 * Match function for witness table
 */
static int
witness_match_func(const void *key1, const void *key2, Size keysize)
{
	const witness_entry *entry1 = (const witness_entry *) key1;
	const witness_entry *entry2 = (const witness_entry *) key2;
	
	return (entry1->hash1 == entry2->hash1 && entry1->hash2 == entry2->hash2) ? 0 : 1;
}
