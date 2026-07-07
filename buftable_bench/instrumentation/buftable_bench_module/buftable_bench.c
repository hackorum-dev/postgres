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

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(buftable_bench_probe);

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
