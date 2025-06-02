/*-------------------------------------------------------------
 *
 * rwoptspin.h
 *     Read-Write optimistic spin lock.
 *
 * It works best when there are few writers and a lot of simultaneous
 * readers.
 *
 * Synchronization primitive relied on lock version:
 * - to acquire write lock, writer first makes version odd with atomic
 * operation (and spins if it is already odd),
 * - to release write lock, writer increments version, therefore version become
 * even,
 * - reader ensures version is even (spin-waiting in other case), then reads
 * protected values "lockless".
 * - after read version should be rechecked. If it changed, read should be
 * retried.
 *
 * Reader could perform only simple read operations. It should not perform any
 * write operation in its loop nor complex reads which may be invalid in case
 * of concurrent changes.
 */

#ifndef RWSPIN_H
#define RWSPIN_H

#include "c.h"
#include "port/atomics.h"

#if defined(PG_HAVE_ATOMIC_U64_SUPPORT) && !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
#define PG_RWOPTSPIN_NATIVE 1

typedef pg_atomic_uint64 RWOptSpin;

/* Initialize RWSpin. */
static inline void RWOptSpinInit(RWOptSpin *spin);

/* Acquire RWOptSpin for write operation. */
#define RWOptSpinAcquire(spin) RWOptSpinAcquire_impl((spin), __FILE__, __LINE__, __func__)

/* Release write lock of RWOptSpin. */
static inline void RWOptSpinRelease(RWOptSpin *spin);

/*-----------------------------------------
 * Syntax sugar for read operations.
 * Example usage:
 *
 *    RWOptSpinReadDo(rwspin);
 *    val1 = read_protected_value1();
 *    val2 = read_protected_value2();
 *    RWOptSpinReadWhile(rwspin);
 */
#define RWOptSpinReadDo(rwspin) \
	{ uint64 rwopt_version = RWOptSpinReadStart((rwspin), __FILE__, __LINE__, __func__); do {
#define RWOptSpinReadWhile(rwspin) \
	} while (RWOptSpinRetryRead((rwspin), &rwopt_version, __FILE__, __LINE__, __func__)); }

/* Implementation */

static inline void
RWOptSpinInit(RWOptSpin *spin)
{
	pg_atomic_init_u64(spin, 0);
}

extern void RWOptSpinAcquire_slowpath(RWOptSpin *spin, const char *file, int line, const char *func);

static inline void
RWOptSpinAcquire_impl(RWOptSpin *spin, const char *file, int line, const char *func)
{
	if (likely((pg_atomic_fetch_or_u64(spin, 1) & 1) == 0))
		return;
	RWOptSpinAcquire_slowpath(spin, file, line, func);
}

static inline void
RWOptSpinRelease(RWOptSpin *spin)
{
	pg_atomic_add_fetch_u64(spin, 1);
}

extern void RWOptSpinRead_wait(RWOptSpin *spin, uint64 *version, const char *file, int line, const char *func);

static inline uint64
RWOptSpinReadStart(RWOptSpin *spin, const char *file, int line, const char *func)
{
	uint64		version = pg_atomic_read_u64(spin);

	/* If it is write locked, wait until writer finished its work */
	if (version & 1)
		RWOptSpinRead_wait(spin, &version, file, line, func);

	/*
	 * Memory barrier is to provide both acquire+release semantic and read
	 * barrier between version read and actual reads.
	 */
	pg_memory_barrier();

	return version;
}

static inline bool
RWOptSpinRetryRead(RWOptSpin *spin, uint64 *version, const char *file, int line, const char *func)
{
	uint64		cur;

	pg_read_barrier();
	cur = pg_atomic_read_u64(spin);
	if (cur == *version)
		return false;

	*version = cur;

	/* If it is write locked, wait until writer finished its work */
	if (*version & 1)
		RWOptSpinRead_wait(spin, version, file, line, func);

	pg_read_barrier();
	return true;
}

#else							/* defined(PG_HAVE_ATOMIC_U64_SUPPORT) &&
								 * !defined(PG_HAVE_ATOMIC_U64_SIMULATION) */

#include "storage/spin.h"

typedef slock_t RWOptSpin;

#define RWOptSpinInit(lock) SpinLockInit(lock)
#define RWOptSpinAcquire(lock) SpinLockAcquire(lock)
#define RWOptSpinRelease(lock) SpinLockRelease(lock)

#define RWOptSpinReadDo(lock) SpinLockAcquire(lock)
#define RWOptSpinReadWhile(lock) SpinLockRelease(lock)

#endif							/* defined(PG_HAVE_ATOMIC_U64_SUPPORT) &&
								 * !defined(PG_HAVE_ATOMIC_U64_SIMULATION) */

#endif
