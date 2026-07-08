/*-------------------------------------------------------------------------
 *
 * buftable_bench.c
 *		Pollution-free in-place benchmark of the shared buffer mapping table.
 *
 * Throwaway micro-benchmark module (NOT for upstream).  One SQL function,
 * buftable_bench_probe(n, rounds), times lookup (hit+miss), insert, and delete
 * by calling BufTable{Insert,Lookup,Delete} DIRECTLY on the real shared table
 * -- no ReadBuffer, no 8 KB page copy, no per-op timing.  Each op's loop is
 * bulk-timed with a single instr_time pair (RDTSC on x86 when available), so
 * the measurement isn't polluted by page-copy cache traffic or per-op timer
 * overhead.
 *
 * It works against STOCK PostgreSQL: it only calls the existing public
 * BufTable* / BufTableHashCode functions, so no core changes are needed -- the
 * two arms being compared are just two stock builds (flat table vs dynahash).
 *
 * Insert/delete mutate the live table, so we only use FREE buffer slots (their
 * mapping entry is guaranteed empty) and restore the table afterward.
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
#include "portability/instr_time.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "common/pg_prng.h"

#include "no_optimise.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(buftable_bench_probe);

/*
 * Auxiliary macros for readability
 */
static int64 timing_operation_id = 0;
#define BEGIN_TIMING(name, n) \
{\
	instr_time t0, t1, dt; \
	Datum		values[4]; \
	bool		nulls[4] = {0}; \
	values[0] = CStringGetTextDatum(name); \
	INSTR_TIME_SET_CURRENT_FAST(t0); \
	for(int64 i = 0; i < n; ++i) \
	{

#define END_TIMING \
	} \
	INSTR_TIME_SET_CURRENT_FAST(t1); \
	INSTR_TIME_SET_ZERO(dt); \
	INSTR_TIME_ACCUM_DIFF(dt, t1, t0); \
	values[1] = Float8GetDatum((double)INSTR_TIME_GET_NANOSEC(dt) / (double) (n));	\
	values[2] = Int64GetDatum(n); \
	values[3] = Int64GetDatum(++timing_operation_id); \
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls); \
}

/* synthetic relfilenodes for bench tags — unlikely to collide with anything real */
#define BENCH_SPC_OID  0xB0B0
#define BENCH_DB_OID   0xB1B1
#define BENCH_REL_PRESENT  ((RelFileNumber) 0x7E570001)
#define BENCH_REL_ABSENT   ((RelFileNumber) 0x7E570002)


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
	int32	   *r_ord,
			   *w_ord;
	BufferTag  *ptag,
			   *atag;
	uint64	   *phash,
			   *ahash;
	int64		nfree = 0;
	volatile int64 sink = 0;
	RelFileLocator rp = {.spcOid = BENCH_SPC_OID, .dbOid = BENCH_DB_OID, .relNumber = BENCH_REL_PRESENT};
	RelFileLocator ra = {.spcOid = BENCH_SPC_OID, .dbOid = BENCH_DB_OID, .relNumber = BENCH_REL_ABSENT};

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
	phash = palloc(sizeof(uint64) * n);
	ahash = palloc(sizeof(uint64) * n);
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
	r_ord = palloc(sizeof(int64) * n);
	w_ord = palloc(sizeof(int64) * n);
	for (int64 i = 0; i < n; i++)
	{
		r_ord[i] = i;
		w_ord[i] = i;
	}
	if (randomize)
	{
		pg_prng_state rng;

		pg_prng_seed(&rng, 0x9E3779B97F4A7C15ULL);

		for (int64 i = n - 1; i > 0; i--)
		{
			int32		k;
			int32		tmp;

			k = pg_prng_int64_range(&rng, 0, i);
			tmp = r_ord[i];
			r_ord[i] = r_ord[k];
			r_ord[k] = tmp;

			k = pg_prng_int64_range(&rng, 0, i);
			tmp = w_ord[i];
			w_ord[i] = w_ord[k];
			w_ord[k] = tmp;

			k = pg_prng_int64_range(&rng, 0, i);
			tmp = bufids[i];
			bufids[i] = bufids[k];
			bufids[k] = tmp;
		}

	}

	if (!timing_initialized)
		pg_initialize_timing();

	PG_TRY();
	{
		for (int64 r = 0; r < rounds; r++)
		{
			timing_operation_id = 0;
			BEGIN_TIMING("insert", n)
			{
				int32		j = w_ord[i];
				BufTableInsert(&ptag[j], phash[j], bufids[j]);
			}
			END_TIMING

			BEGIN_TIMING("hit", n)
			{
				int32		j = r_ord[i];
				sink += BufTableLookup(&ptag[j], phash[j]);
			}
			END_TIMING;

			BEGIN_TIMING("miss", n)
			{
				int32		j = r_ord[i];
				sink += BufTableLookup(&atag[j], ahash[j]);
			}
			END_TIMING

			BEGIN_TIMING("del-tag", n)
			{
				int32		j = r_ord[i];
				BufTableDelete(&ptag[j], phash[j]);
			}
			END_TIMING

			BEGIN_TIMING("LWLock-ex", n)
			{
				int32		j = r_ord[i];
				LWLock	   *partition_lock;

				partition_lock = BufMappingPartitionLock(phash[j]);
				LWLockAcquire(partition_lock, LW_EXCLUSIVE);
				LWLockRelease(partition_lock);
			}
			END_TIMING

			BEGIN_TIMING("LWLock", n)
			{
				int32		j = r_ord[i];
				LWLock	   *partition_lock;

				partition_lock = BufMappingPartitionLock(phash[j]);
				LWLockAcquire(partition_lock, LW_SHARED);
				LWLockRelease(partition_lock);
			}
			END_TIMING

			BEGIN_TIMING("HdrLock", n)
			{
				int32		j = r_ord[i];
				BufferDesc *desc = GetBufferDescriptor(bufids[j] - 1);

				LockBufHdr(desc);
				UnlockBufHdr(desc);
			}
			END_TIMING

			BEGIN_TIMING("hash", n)
			{
				int32		j = r_ord[i];
				sink += BufTableHashCode(&ptag[j]);
			}
			END_TIMING

			BEGIN_TIMING("compare", n)
			{
				int32		j = r_ord[i];
				sink += ext_BufferTagsEqual(&ptag[j], &ptag[i]);
			}
			END_TIMING

			BEGIN_TIMING("nop", n)
			{
				int32		j = r_ord[i];
				ext_nop(&ptag[j], phash[j]);
			}
			END_TIMING
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
	(void) sink;
	return (Datum) 0;
}
