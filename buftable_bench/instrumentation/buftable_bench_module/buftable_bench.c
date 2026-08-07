/*-------------------------------------------------------------------------
 *
 * buftable_bench.c
 *		Pollution-free in-place benchmark of the shared buffer mapping table.
 *
 * Throwaway micro-benchmark module (NOT for upstream).  One SQL function,
 * buftable_bench_probe(n, rounds), times lookup (hit+miss), insert, and delete
 * by calling BufTable{Insert,Lookup,Delete} DIRECTLY on the real shared table
 * -- no ReadBuffer, no 8 KB page copy, no per-op rdtsc.  Each op's loop is
 * bulk-timed with a single rdtsc pair, so the measurement isn't polluted by
 * page-copy cache traffic or per-op timer overhead.
 *
 * It works against STOCK PostgreSQL: it only calls the existing public
 * BufTable* / BufTableHashCode functions, so no core changes are needed -- the
 * two arms being compared are just two stock builds (flat table vs dynahash).
 *
 * Insert/delete mutate the live table, so we only use FREE buffer slots (their
 * mapping entry is guaranteed empty) and restore the table afterward.
 *
 * There are two functions:
 *
 *	  buftable_bench_probe(n, rounds, random)
 *		load-factor ~1 (chains ~1 entry): insert / lookup_hit / lookup_miss /
 *		delete, each bulk-timed.
 *
 *	  buftable_bench_collide(chain_len, total_entries, rounds, random,
 *							   delete_under_lock)
 *		WORST-CASE hash-collision test: forces many tags into the same bucket
 *		chain (length chain_len) and times lookup_hit_full + delete_drain.  This
 *		exists to measure the concern that the flat table's BufTableDelete()
 *		runs while the buffer-header spinlock is held in InvalidateBuffer()
 *		(bufmgr.c), so its chain-walk cost IS extra spinlock-hold time -- unlike
 *		the dynahash baseline, which deleted after releasing the spinlock.  The
 *		delete_drain per-op ns is exactly that added hold; sweeping chain_len
 *		shows how it scales with collisions.  It ALSO reports lock_hold: the
 *		actual average time the real buffer-header spinlock is held across a
 *		replica of the InvalidateBuffer critical section, with BufTableDelete
 *		inside the hold (delete_under_lock=true, flat) or after the unlock
 *		(false, origin/dynahash) -- the total hold, of which delete_drain is the
 *		flat-added component.
 *
 * x86_64 only (rdtsc).
 *
 * IDENTIFICATION
 *	  src/test/modules/buftable_bench/buftable_bench.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "portability/instr_time.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(buftable_bench_probe);
PG_FUNCTION_INFO_V1(buftable_bench_collide);

#define BUFTABLE_BENCH_PROBE_COLS 3

/* synthetic relfilenodes for bench tags — unlikely to collide with anything real */
#define BENCH_SPC_OID  0xB0B0
#define BENCH_DB_OID   0xB1B1
#define BENCH_REL_PRESENT  ((RelFileNumber) 0x7E570001)
#define BENCH_REL_ABSENT   ((RelFileNumber) 0x7E570002)

static inline uint64
bench_rdtsc(void)
{
	uint32		lo,
				hi;

	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)::"memory");
	return ((uint64) hi << 32) | lo;
}

/* cycles per nanosecond, measured over a ~2 ms wall-clock window */
static double
probe_calibrate(void)
{
	instr_time	w0,
				w1,
				d;
	uint64		c0,
				c1;
	double		ns;

	INSTR_TIME_SET_CURRENT(w0);
	c0 = bench_rdtsc();
	do
	{
		INSTR_TIME_SET_CURRENT(w1);
		d = w1;
		INSTR_TIME_SUBTRACT(d, w0);
	} while (INSTR_TIME_GET_DOUBLE(d) < 0.002);
	c1 = bench_rdtsc();
	ns = INSTR_TIME_GET_DOUBLE(d) * 1e9;
	return (ns > 0.0) ? (double) (c1 - c0) / ns : 0.0;
}

/*
 * Collect up to want FREE buffer slots (mapping entry guaranteed empty, so we
 * can insert a synthetic tag for that buf_id and restore it by delete).  Fills
 * bufids[] and returns the count actually found.
 */
static int64
bench_collect_free_bufids(int *bufids, int64 want)
{
	int64		nfree = 0;

	for (int i = 0; i < NBuffers && nfree < want; i++)
	{
		BufferDesc *desc = GetBufferDescriptor(i);
		uint64		state = pg_atomic_read_u64(&desc->state);

		if (!(state & BM_TAG_VALID))
			bufids[nfree++] = i;
	}
	return nfree;
}

/*
 * Fill ord[0..n) with a visit order over [0, n): identity when randomize is
 * false, else a Fisher-Yates shuffle seeded by `seed` (reproducible).
 *
 * The insert and delete phases must use INDEPENDENT permutations (different
 * seeds).  The two arms lay chains out in opposite physical order -- flat
 * head-inserts (buf_table.c), dynahash tail-appends (dynahash.c) -- so if the
 * delete order matched the insert order, flat would always delete the tail-most
 * entry (walk the whole chain, its worst case) while dynahash always deleted the
 * head (O(1), its best case), and the comparison would be an artifact of that
 * correlation rather than the per-node walk cost.  An independent delete order
 * makes the deleted entry's chain position uniform for BOTH arms, so each
 * samples the same ~(len+1)/2 expected walk -- the fair comparison.
 */
static void
bench_make_order(int64 *ord, int64 n, bool randomize, uint64 seed)
{
	for (int64 i = 0; i < n; i++)
		ord[i] = i;

	if (randomize)
	{
		uint64		rng = seed;

		for (int64 i = n - 1; i > 0; i--)
		{
			int64		k,
						tmp;

			rng ^= rng << 13;
			rng ^= rng >> 7;
			rng ^= rng << 17;
			k = (int64) (rng % (uint64) (i + 1));
			tmp = ord[i];
			ord[i] = ord[k];
			ord[k] = tmp;
		}
	}
}

/*
 * buftable_bench_probe(n, rounds) -> SETOF (op text, avg_ns float8, count int8)
 *
 * Rows: insert, lookup_hit, lookup_miss, delete.  See file header.
 */
Datum
buftable_bench_probe(PG_FUNCTION_ARGS)
{
	int64		n = PG_GETARG_INT64(0);
	int64		rounds = PG_ARGISNULL(1) ? 1 : PG_GETARG_INT64(1);
	bool		randomize = PG_ARGISNULL(2) ? true : PG_GETARG_BOOL(2);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int		   *bufids;
	int64	   *ord;
	BufferTag  *ptag,
			   *atag;
	uint32	   *phash,
			   *ahash;
	int64		nfree = 0;
	double		cyc_per_ns,
				denom;
	uint64		ins = 0,
				lkh = 0,
				lkm = 0,
				del = 0;
	volatile int64 sink = 0;
	RelFileLocator rp = {.spcOid = BENCH_SPC_OID,.dbOid = BENCH_DB_OID,.relNumber = BENCH_REL_PRESENT};
	RelFileLocator ra = {.spcOid = BENCH_SPC_OID,.dbOid = BENCH_DB_OID,.relNumber = BENCH_REL_ABSENT};
	const char *names[4] = {"insert", "lookup_hit", "lookup_miss", "delete"};
	double		avg[4];

	if (n <= 0 || rounds <= 0)
		ereport(ERROR, (errmsg("n and rounds must be positive")));

	InitMaterializedSRF(fcinfo, 0);

	/* collect up to n FREE buffer slots (mapping entry guaranteed empty) */
	bufids = palloc(sizeof(int) * n);
	for (int i = 0; i < NBuffers && nfree < n; i++)
	{
		BufferDesc *desc = GetBufferDescriptor(i);
		uint64		state = pg_atomic_read_u64(&desc->state);

		if (!(state & BM_TAG_VALID))
			bufids[nfree++] = i;
	}
	n = nfree;
	if (n == 0)
		ereport(ERROR, (errmsg("no free buffers to probe with")));

	/* build present + absent tags and their hashes */
	ptag = palloc(sizeof(BufferTag) * n);
	atag = palloc(sizeof(BufferTag) * n);
	phash = palloc(sizeof(uint32) * n);
	ahash = palloc(sizeof(uint32) * n);
	for (int64 j = 0; j < n; j++)
	{
		InitBufferTag(&ptag[j], &rp, MAIN_FORKNUM, (BlockNumber) j);
		InitBufferTag(&atag[j], &ra, MAIN_FORKNUM, (BlockNumber) j);
		phash[j] = BufTableHashCode(&ptag[j]);
		ahash[j] = BufTableHashCode(&atag[j]);
	}

	/*
	 * Iteration order over the keys: identity (sequential) or a Fisher-Yates
	 * shuffle (random).  A shuffled order makes the timed loops visit keys in
	 * an order uncorrelated with where their entries/elements live, so BOTH
	 * arms' entry/element access is random (not just the bucket access, which
	 * the hash already scatters).  Done once in setup (untimed).
	 */
	ord = palloc(sizeof(int64) * n);
	for (int64 i = 0; i < n; i++)
		ord[i] = i;
	if (randomize)
	{
		uint64		rng = 0x9E3779B97F4A7C15ULL;	/* fixed seed -> reproducible */

		for (int64 i = n - 1; i > 0; i--)
		{
			int64		k,
						tmp;

			rng ^= rng << 13;
			rng ^= rng >> 7;
			rng ^= rng << 17;
			k = (int64) (rng % (uint64) (i + 1));
			tmp = ord[i];
			ord[i] = ord[k];
			ord[k] = tmp;
		}
	}

	cyc_per_ns = probe_calibrate();

	PG_TRY();
	{
		for (int64 r = 0; r < rounds; r++)
		{
			uint64		t0,
						t1;

			t0 = bench_rdtsc();
			for (int64 i = 0; i < n; i++)
			{
				int64		j = ord[i];

				BufTableInsert(&ptag[j], phash[j], bufids[j]);
			}
			t1 = bench_rdtsc();
			ins += t1 - t0;

			t0 = bench_rdtsc();
			for (int64 i = 0; i < n; i++)
			{
				int64		j = ord[i];

				sink += BufTableLookup(&ptag[j], phash[j]);
			}
			t1 = bench_rdtsc();
			lkh += t1 - t0;

			t0 = bench_rdtsc();
			for (int64 i = 0; i < n; i++)
			{
				int64		j = ord[i];

				sink += BufTableLookup(&atag[j], ahash[j]);
			}
			t1 = bench_rdtsc();
			lkm += t1 - t0;

			t0 = bench_rdtsc();
			for (int64 i = 0; i < n; i++)
			{
				int64		j = ord[i];

				BufTableDelete(&ptag[j], phash[j]);
			}
			t1 = bench_rdtsc();
			del += t1 - t0;
		}
	}
	PG_CATCH();
	{
		/* best-effort restore: remove any present tag still mapped */
		for (int64 j = 0; j < n; j++)
			if (BufTableLookup(&ptag[j], phash[j]) >= 0)
				BufTableDelete(&ptag[j], phash[j]);
		PG_RE_THROW();
	}
	PG_END_TRY();

	denom = (double) n * (double) rounds * cyc_per_ns;
	avg[0] = ins / denom;
	avg[1] = lkh / denom;
	avg[2] = lkm / denom;
	avg[3] = del / denom;

	for (int i = 0; i < 4; i++)
	{
		Datum		values[BUFTABLE_BENCH_PROBE_COLS];
		bool		nulls[BUFTABLE_BENCH_PROBE_COLS] = {0};

		values[0] = CStringGetTextDatum(names[i]);
		values[1] = Float8GetDatum(avg[i]);
		values[2] = Int64GetDatum(n * rounds);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	(void) sink;
	return (Datum) 0;
}

/*
 * buftable_bench_collide(chain_len, total_entries, rounds, random)
 *		-> SETOF (op text, avg_ns float8, count int8)
 *
 * Worst-case hash-collision benchmark.  Forces total_entries synthetic tags
 * into K = total_entries/chain_len bucket chains, each of length chain_len (G),
 * then bulk-times two ops over all K*G present keys, in a randomized visit
 * order, per round:
 *
 *	  lookup_hit_full  BufTableLookup on every key with chains intact.  Pure
 *					   chain-walk (no unlink); avg (G+1)/2 BufferTagsEqual compares
 *					   per lookup.  Its ns-vs-G slope is the per-comparison cost and
 *					   is the collision sanity check (must grow ~linearly in G).
 *	  delete_drain	   BufTableDelete on every key, emptying the chains.  This is
 *					   the work the flat table now does under the buffer-header
 *					   spinlock in InvalidateBuffer(), so its per-op ns is the extra
 *					   spinlock-hold time the restructuring adds (dynahash deleted
 *					   after releasing the spinlock, so its hold contribution is 0).
 *
 * Rows returned: insert_build, lookup_hit_full, delete_drain, and
 * worst_delete_est (a DERIVED estimate = per-comparison ns * G, i.e. the cost of
 * deleting the tail-most entry which walks the whole chain -- reported so the
 * random-order average is never misread as the worst case).
 *
 * Collisions are constructed to land in the SAME bucket in BOTH the flat and the
 * dynahash arm: both compute the identical hashcode (tag_hash over the tag), and
 * both index the bucket with the low bits of that hashcode.  We keep tags whose
 * hashcode shares the low B bits, with 2^B >= both arms' bucket counts, so they
 * collide regardless of each arm's exact bucketing.  B is derived from NBuffers
 * alone, so it is identical in both arms.
 *
 * Like buftable_bench_probe, this uses only FREE buffer slots and restores the
 * table via PG_TRY.  It calls BufTable* directly, single-backend, holding no
 * partition locks (safe: no other backend touches these synthetic tags).
 *
 * x86_64 only (rdtsc).
 */
Datum
buftable_bench_collide(PG_FUNCTION_ARGS)
{
	int64		G = PG_GETARG_INT64(0);
	int64		req_total = PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1);
	int64		rounds = PG_ARGISNULL(2) ? 10 : PG_GETARG_INT64(2);
	bool		randomize = PG_ARGISNULL(3) ? true : PG_GETARG_BOOL(3);
	bool		delete_under_lock = PG_ARGISNULL(4) ? true : PG_GETARG_BOOL(4);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int		   *bufids;
	int64	   *ord;
	int64	   *dord;
	BufferTag  *ptag;
	uint32	   *phash;
	int64		nfree,
				E,
				K,
				total;
	int			B;
	uint32		mod_mask;
	int			cap_total;
	/* per-residue fill state for the collision search */
	int		   *res_count;		/* how many tags collected for each residue */
	int		   *res_slot;		/* group index for a residue: -1 unseen, -2 ignore */
	bool	   *slot_full;		/* slot_full[s] set once group s reaches G */
	int64		groups_started = 0;
	int64		ngroups = 0;
	double		cyc_per_ns,
				denom;
	uint64		ins = 0,
				lkh = 0,
				del = 0,
				hold = 0;
	volatile int64 sink = 0;
	BufferTag	dummy;			/* scratch target for ClearBufferTag under the lock */
	RelFileLocator rp = {.spcOid = BENCH_SPC_OID,.dbOid = BENCH_DB_OID,.relNumber = BENCH_REL_PRESENT};
	const char *names[5] = {"insert_build", "lookup_hit_full", "delete_drain", "worst_delete_est", "lock_hold"};
	double		avg[5];

	if (G <= 0 || rounds <= 0)
		ereport(ERROR, (errmsg("chain_len and rounds must be positive")));

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Collision modulus.  Both arms bucket on the low bits of the (identical)
	 * hashcode.  Flat uses num_buckets = Max(NUM_BUFFER_PARTITIONS,
	 * pg_nextpower2_32(NBuffers)); dynahash uses ~next_pow2(NBuffers +
	 * NUM_BUFFER_PARTITIONS) and consults up to the low log2(2*nbuckets) bits.
	 * B = ceil_log2(Max(NBuffers,128)) + 3 makes 2^B comfortably exceed both, so
	 * tags equal in the low B bits collide in the same bucket in BOTH arms.
	 */
	B = pg_ceil_log2_32((uint32) Max(NBuffers, 128)) + 3;

	/*
	 * The residue-bookkeeping arrays below are sized 2^B.  B grows with NBuffers
	 * (~log2(shared_buffers)); at the sizes this tool targets (<= a few tens of
	 * GB) B is <= 26, i.e. arrays <= ~0.5 GB.  Refuse absurdly large pools rather
	 * than clamp B, which would drop 2^B below an arm's bucket count and silently
	 * break the collision guarantee.
	 */
	if (B > 26)
		ereport(ERROR,
				(errmsg("shared_buffers too large for buftable_bench_collide (B=%d)", B),
				 errhint("This throwaway benchmark targets pools up to a few tens of GB.")));
	mod_mask = (1u << B) - 1;

	/*
	 * Target total entries.  Auto/default = min(NBuffers/2, 65536): enough for a
	 * stable per-op mean (E*rounds samples) while keeping the collision search
	 * and per-round loops fast and the free-buffer footprint modest.  An explicit
	 * total_entries is still capped at NBuffers/2 (must fit in free slots).
	 */
	cap_total = (int) Max((int64) 1, (int64) NBuffers / 2);
	E = (req_total > 0) ? req_total : Min((int64) cap_total, (int64) 65536);
	if (E > cap_total)
		E = cap_total;
	if (E < G)
		ereport(ERROR, (errmsg("total_entries (%ld) must be >= chain_len (%ld)", (long) E, (long) G)));

	/* collect free buffer slots to host the entries */
	bufids = palloc(sizeof(int) * E);
	nfree = bench_collect_free_bufids(bufids, E);
	if (nfree < G)
		ereport(ERROR, (errmsg("only %ld free buffers; need at least chain_len=%ld",
							   (long) nfree, (long) G)));
	if (nfree < E)
	{
		ereport(WARNING, (errmsg("only %ld free buffers available; reducing total_entries from %ld",
								 (long) nfree, (long) E)));
		E = nfree;
	}

	/* K full groups of size G fit into E slots */
	K = E / G;
	if (K < 1)
		K = 1;
	total = K * G;

	/*
	 * Brute-force search for K residues (low-B-bit values) that each accumulate
	 * G distinct tags.  We vary only blockNum; all other tag fields are fixed,
	 * so distinct blockNum -> distinct tag.  We "start" a group the first time we
	 * see a residue (up to K groups); a residue seen after K groups are started
	 * is ignored.  Each candidate is written straight into its group's slice of
	 * ptag[], so a completed group occupies ptag[slot*G .. slot*G+G).
	 */
	res_count = palloc0(sizeof(int) * ((Size) mod_mask + 1));
	res_slot = palloc(sizeof(int) * ((Size) mod_mask + 1));
	for (int64 r = 0; r <= (int64) mod_mask; r++)
		res_slot[r] = -1;
	slot_full = palloc0(sizeof(bool) * K);

	ptag = palloc(sizeof(BufferTag) * total);
	phash = palloc(sizeof(uint32) * total);

	{
		/*
		 * Scan cap: on average we need G*2^B candidates to fill K groups, but
		 * groups compete for residues, so allow a generous multiple.  If we run
		 * out, reduce K to what we filled.
		 */
		uint64		scanned = 0;
		uint64		scan_cap = (uint64) (mod_mask + 1) * (uint64) G * 8 + 1000000;
		BlockNumber blk = 0;

		while (ngroups < K && scanned < scan_cap)
		{
			BufferTag	tag;
			uint32		h,
						res;
			int			slot;
			int64		idx;

			scanned++;
			if (blk == P_NEW)		/* skip InvalidBlockNumber */
			{
				blk++;
				continue;
			}
			InitBufferTag(&tag, &rp, MAIN_FORKNUM, blk);
			blk++;

			h = BufTableHashCode(&tag);
			res = h & mod_mask;

			slot = res_slot[res];
			if (slot == -2)
				continue;			/* residue's group already full, ignore */
			if (slot == -1)
			{
				/* first sighting of this residue: start a new group if we can */
				if (groups_started >= K)
				{
					res_slot[res] = -2;
					continue;
				}
				slot = (int) groups_started++;
				res_slot[res] = slot;
			}

			/* append this tag as the next member of group `slot` */
			idx = (int64) slot * G + res_count[res];
			ptag[idx] = tag;
			phash[idx] = h;
			res_count[res]++;

			if (res_count[res] == (int) G)
			{
				res_slot[res] = -2; /* group full; ignore further hits */
				slot_full[slot] = true;
				ngroups++;
			}
		}

		if (ngroups < K)
		{
			ereport(WARNING, (errmsg("collision search filled only %ld of %ld groups (chain_len=%ld); reducing",
									 (long) ngroups, (long) K, (long) G)));

			/* compact the full groups to the front of ptag[]/phash[] */
			{
				int64		dst = 0;

				for (int64 s = 0; s < groups_started; s++)
				{
					if (!slot_full[s])
						continue;
					if (dst != s)
					{
						memcpy(&ptag[dst * G], &ptag[s * G], sizeof(BufferTag) * G);
						memcpy(&phash[dst * G], &phash[s * G], sizeof(uint32) * G);
					}
					dst++;
				}
			}
			K = ngroups;
			total = K * G;
		}
	}

	if (total <= 0)
		ereport(ERROR, (errmsg("could not build any full collision chain of length %ld", (long) G)));

	/*
	 * Two independent visit orders over all present keys: `ord` for the
	 * insert/lookup phases, `dord` (different seed) for the delete phase, so the
	 * deleted entry's chain position is uncorrelated with its insertion position
	 * -- fair to both arms (see bench_make_order).
	 */
	ord = palloc(sizeof(int64) * total);
	dord = palloc(sizeof(int64) * total);
	bench_make_order(ord, total, randomize, 0x9E3779B97F4A7C15ULL);
	bench_make_order(dord, total, randomize, 0xD1B54A32D192ED03ULL);

	cyc_per_ns = probe_calibrate();

	PG_TRY();
	{
		for (int64 r = 0; r < rounds; r++)
		{
			uint64		t0,
						t1;

			/* build: insert every key (timed for reference, not the headline) */
			t0 = bench_rdtsc();
			for (int64 i = 0; i < total; i++)
			{
				int64		j = ord[i];

				BufTableInsert(&ptag[j], phash[j], bufids[j]);
			}
			t1 = bench_rdtsc();
			ins += t1 - t0;

			/* lookup every key with chains fully populated (pure chain-walk) */
			t0 = bench_rdtsc();
			for (int64 i = 0; i < total; i++)
			{
				int64		j = ord[i];

				sink += BufTableLookup(&ptag[j], phash[j]);
			}
			t1 = bench_rdtsc();
			lkh += t1 - t0;

			/* drain: delete every key (independent order), emptying the chains */
			t0 = bench_rdtsc();
			for (int64 i = 0; i < total; i++)
			{
				int64		j = dord[i];

				BufTableDelete(&ptag[j], phash[j]);
			}
			t1 = bench_rdtsc();
			del += t1 - t0;

			/*
			 * lock_hold: measure the ACTUAL buffer-header spinlock hold time of
			 * the InvalidateBuffer() critical section.  The drain above emptied
			 * the chains, so first rebuild them (untimed), then for each key take
			 * the real header spinlock on that key's (free) buffer, run the exact
			 * work InvalidateBuffer holds the lock across, and release -- all
			 * bracketed by one rdtsc pair per key.  With delete_under_lock=true
			 * (flat) BufTableDelete runs INSIDE the hold; with false (origin/
			 * dynahash) it runs AFTER the unlock (untimed here), exactly as the
			 * two InvalidateBuffer variants place it.  Same randomized dord order
			 * as the drain, so both arms sample uniform chain positions.
			 */
			for (int64 i = 0; i < total; i++)	/* rebuild chains, untimed */
			{
				int64		j = ord[i];

				BufTableInsert(&ptag[j], phash[j], bufids[j]);
			}

			for (int64 i = 0; i < total; i++)
			{
				int64		j = dord[i];
				BufferDesc *desc = GetBufferDescriptor(bufids[j]);
				uint64		bstate;

				t0 = bench_rdtsc();
				bstate = LockBufHdr(desc);	/* real header spinlock acquire */
				/* work InvalidateBuffer does under the lock (cost only) */
				sink += BufferTagsEqual(&desc->tag, &ptag[j]);
				sink += (int64) BUF_STATE_GET_REFCOUNT(bstate);
				ClearBufferTag(&dummy); /* local scratch; don't mutate desc->tag */
				if (delete_under_lock)	/* flat: delete inside the hold */
					BufTableDelete(&ptag[j], phash[j]);
				/* unlock with no net state change (only clears BM_LOCKED) */
				UnlockBufHdrExt(desc, bstate, 0, 0, 0);
				t1 = bench_rdtsc();
				hold += t1 - t0;

				if (!delete_under_lock) /* origin: delete AFTER unlock, untimed */
					BufTableDelete(&ptag[j], phash[j]);
			}
		}
	}
	PG_CATCH();
	{
		/* best-effort restore: remove any present tag still mapped */
		for (int64 j = 0; j < total; j++)
			if (BufTableLookup(&ptag[j], phash[j]) >= 0)
				BufTableDelete(&ptag[j], phash[j]);
		PG_RE_THROW();
	}
	PG_END_TRY();

	denom = (double) total * (double) rounds * cyc_per_ns;
	avg[0] = ins / denom;		/* insert_build */
	avg[1] = lkh / denom;		/* lookup_hit_full: avg (G+1)/2 compares */
	avg[2] = del / denom;		/* delete_drain: == flat's extra spinlock hold */

	/*
	 * worst_delete_est: cost of deleting the tail-most entry (walks all G
	 * nodes).  lookup_hit_full averages (G+1)/2 comparisons, so per-comparison
	 * ns = avg[1] / ((G+1)/2); the worst single delete does G comparisons.
	 */
	{
		double		per_cmp = avg[1] / (((double) G + 1.0) / 2.0);

		avg[3] = per_cmp * (double) G;
	}

	/*
	 * lock_hold: average time the buffer-header spinlock was actually held across
	 * the replicated InvalidateBuffer critical section (see the timed phase).
	 * flat lock_hold - origin lock_hold should track delete_drain -- the moved
	 * BufTableDelete is the only work whose lock placement differs.
	 */
	avg[4] = hold / denom;

	for (int i = 0; i < 5; i++)
	{
		Datum		values[BUFTABLE_BENCH_PROBE_COLS];
		bool		nulls[BUFTABLE_BENCH_PROBE_COLS] = {0};

		values[0] = CStringGetTextDatum(names[i]);
		values[1] = Float8GetDatum(avg[i]);
		values[2] = Int64GetDatum(total * rounds);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	(void) sink;
	return (Datum) 0;
}
