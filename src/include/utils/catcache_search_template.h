/*-------------------------------------------------------------------------
 * catcache_search_template.h
 *
 * A template for type-specialized SearchCatCache functions
 *
 * Usage notes:
 *
 * To generate functions specialized for a set of catcache keys,
 * the following parameter macros should be #define'd before this
 * file is included.
 *
 * - CC_SEARCH - the name of the search function to be generated
 * - CC_NKEYS - the number of search keys for the tuple
 * - FASTEQFUNCx (x in 1,2,3,4) - type-specific equality function(s)
 * - HASHFUNCx (x in 1,2,3,4) - type-specific hash function(s)
 *
 *
 * Copyright (c) 2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1992-1994, Regents of the University of California
 *
 * src/include/utils/catcache_search_template.h
 *
 *-------------------------------------------------------------------------
 */


HeapTuple
CC_SEARCH(CatCache *cache,
		  Datum v1,
		  Datum v2,
		  Datum v3,
		  Datum v4)
{
	uint32		hashValue = 0;
	uint32		oneHash;
	Index		hashIndex;
	dlist_iter	iter;
	dlist_head *bucket;
	CatCTup    *ct;

	/* Make sure we're in an xact, even if this ends up being a cache hit */
	Assert(IsTransactionState());

	Assert(cache->cc_nkeys == CC_NKEYS);

	/*
	 * one-time startup overhead for each cache
	 */
	if (unlikely(cache->cc_tupdesc == NULL))
		CatalogCacheInitializeCache(cache);

#ifdef CATCACHE_STATS
	cache->cc_searches++;
#endif

	/*
	 * find the hash bucket in which to look for the tuple
	 */
	CACHE_elog(DEBUG2, "CatalogCacheComputeHashValue %s %d %p",
			   cache->cc_relname, CC_NKEYS, cache);

	switch (CC_NKEYS)
	{
		case 4:
			oneHash = HASHFUNC4(v4);

			hashValue ^= oneHash << 24;
			hashValue ^= oneHash >> 8;
			/* FALLTHROUGH */
		case 3:
			oneHash = HASHFUNC3(v3);

			hashValue ^= oneHash << 16;
			hashValue ^= oneHash >> 16;
			/* FALLTHROUGH */
		case 2:
			oneHash = HASHFUNC2(v2);

			hashValue ^= oneHash << 8;
			hashValue ^= oneHash >> 24;
			/* FALLTHROUGH */
		case 1:
			oneHash = HASHFUNC1(v1);

			hashValue ^= oneHash;
			break;
		default:
			elog(FATAL, "wrong number of hash keys: %d", CC_NKEYS);
			break;
	}

	hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);

	/*
	 * scan the hash bucket until we find a match or exhaust our tuples
	 *
	 * Note: it's okay to use dlist_foreach here, even though we modify the
	 * dlist within the loop, because we don't continue the loop afterwards.
	 */
	bucket = &cache->cc_bucket[hashIndex];
	dlist_foreach(iter, bucket)
	{
		ct = dlist_container(CatCTup, cache_elem, iter.cur);

		if (ct->dead)
			continue;			/* ignore dead entries */

		if (ct->hash_value != hashValue)
			continue;			/* quickly skip entry if wrong hash val */

		switch (CC_NKEYS)
		{
			case 4:
				if (!FASTEQFUNC4(ct->keys[3], v4))
					continue;
				/* FALLTHROUGH */
			case 3:
				if (!FASTEQFUNC3(ct->keys[2], v3))
					continue;
				/* FALLTHROUGH */
			case 2:
				if (!FASTEQFUNC2(ct->keys[1], v2))
					continue;
				/* FALLTHROUGH */
			case 1:
				if (!FASTEQFUNC1(ct->keys[0], v1))
					continue;
				break;
			default:
				elog(FATAL, "wrong number of keys: %d", CC_NKEYS);
				break;
		}

		/*
		 * We found a match in the cache.  Move it to the front of the list
		 * for its hashbucket, in order to speed subsequent searches.  (The
		 * most frequently accessed elements in any hashbucket will tend to be
		 * near the front of the hashbucket's list.)
		 */
		dlist_move_head(bucket, &ct->cache_elem);

		/*
		 * If it's a positive entry, bump its refcount and return it. If it's
		 * negative, we can report failure to the caller.
		 */
		if (!ct->negative)
		{
			ResourceOwnerEnlargeCatCacheRefs(CurrentResourceOwner);
			ct->refcount++;
			ResourceOwnerRememberCatCacheRef(CurrentResourceOwner, &ct->tuple);

			CACHE_elog(DEBUG2, "SearchCatCache(%s): found in bucket %d",
					   cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_hits++;
#endif

			return &ct->tuple;
		}
		else
		{
			CACHE_elog(DEBUG2, "SearchCatCache(%s): found neg entry in bucket %d",
					   cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_neg_hits++;
#endif

			return NULL;
		}
	}

	return SearchCatCacheMiss(cache, CC_NKEYS, hashValue, hashIndex, v1, v2, v3, v4);
}

