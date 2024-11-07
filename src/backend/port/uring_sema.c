/*-------------------------------------------------------------------------
 *
 * uring_sema.c
 *	  Implement PGSemaphores using atomics and futexes operated by IO_URING
 *	  Linux kernel facility
 *
 * We hide futex uaddr and value under typedef PGSemaphore.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/uring_sema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/futex.h>
#include <liburing.h>
#include <sys/syscall.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"
#include "storage/shmem.h"

#if !defined(USE_LIBURING)
#error assert: we shouldnt use uring_sema.c if USE_LIBURING is not defined
#endif

#if defined(USE_LIBURING) && defined(EXEC_BACKEND)
#error cannot use named POSIX semaphores with EXEC_BACKEND
#endif

#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U32 0x02
#endif

typedef struct {
	unsigned int	futex;
	int		value;
}		futex_sem_t;

typedef union SemTPadded {
	futex_sem_t	sem;
	char		pad[PG_CACHE_LINE_SIZE - sizeof(futex_sem_t)];
}		SemTPadded;

/* typedef PGSemaphore is equivalent to pointer to sem_t */
typedef struct PGSemaphoreData {
	SemTPadded	sem_padded;
}		PGSemaphoreData;

#define FUTEX_SEM_REF(x)	(&(x)->sem_padded.sem)
static PGSemaphore sharedSemas;	/* array of PGSemaphoreData in shared memory */
static int	numSems;	/* number of semas acquired so far */
static int	maxSems;	/* allocated size of above arrays */

/* each process may need to initialize this to be able to submit */
static bool	ring_initialized = false;
static struct io_uring ring;

static int
futex(unsigned int *uaddr, int futex_op, int val,
      const struct timespec *timeout, unsigned int *uaddr2,
      unsigned int val3)
{
	return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

static void
fwait(futex_sem_t * s)
{
	long		r;
	int		value = __atomic_sub_fetch(&s->value, 1, __ATOMIC_RELAXED);
	if (value < 0) {
		r = futex(&s->futex, FUTEX_WAIT, s->futex, NULL, 0, 0);
		if (r == -1 && errno != EAGAIN)
			elog(PANIC, "futex-FUTEX_WAIT: %m");
	}
}

static void
fpost(futex_sem_t * s)
{
	long		r;
	int		value = __atomic_add_fetch(&s->value, 1, __ATOMIC_RELAXED);
	if (value <= 0) {
		do {
			r = futex(&s->futex, FUTEX_WAKE, 1, NULL, 0, 0);
			if (r == -1 && errno != EAGAIN)
				elog(PANIC, "futex-FUTEX_WAKE: %m");
		} while (r < 1);
	}
}

/*
 * sem_post() using userspace int and futex using single submission to
 * io_uring
 */
static void
fpost_uring(struct io_uring *ring, futex_sem_t * s)
{
	int		ret;
	struct io_uring_sqe *sqe;
	int		value = __atomic_add_fetch(&s->value, 1, __ATOMIC_RELAXED);
	if (value <= 0) {
		int		c = 0;
		do {
			sqe = io_uring_get_sqe(ring);
			if (sqe)
				break;

			/*
			 * mostly debug code: had terrible experiences
			 * here...
			 */
			sched_yield();
			ret = io_uring_submit(ring);
			if (ret <= 0) {
				if (ret == -EAGAIN || ret == -EINTR)
					continue;
			}
			if (c++ > 10) {
				elog(INFO,
				     "unable to get new SQE, but io_uring_submit() got rc=%d(%s), io_uring_sq_space_left()=%d",
				     ret, strerror(-ret), io_uring_sq_space_left(ring));
				c = 0;
			}
		} while (1);

		io_uring_prep_futex_wake(sqe, &s->futex, 1, FUTEX_BITSET_MATCH_ANY,
					 FUTEX2_SIZE_U32, 0);
		do {
			ret = io_uring_submit_and_get_events(ring);
			/*
			 * mostly debug code: had terrible experiences
			 * here...
			 */
			if (ret == 1)
				break;
			else if (ret < 0 && (ret == -EAGAIN || ret == -EINTR))
				continue;
			else
				elog(INFO,
				     "failed single SQE submission, rc=%d(%s), io_uring_sq_space_left()=%d",
				     ret, strerror(-ret), io_uring_sq_space_left(ring));
		} while (1);

	}
}

/*
 * there's no io_uring_prep_futex_wakev(ector), but we submit bunch of
 * wakeups in one system call
 */
static void
fpost_uring_many(struct io_uring *ring, futex_sem_t * *futarr,
		 int n)
{
	int		ret, i, submitted = 0;
	struct io_uring_sqe *sqe;

	/* for each sem .. */
	for (i = 0; i < n; i++) {
		int		value =
			__atomic_add_fetch(&futarr[i]->value, 1, __ATOMIC_RELAXED);
		if (value <= 0) {
			/* allocate only if we dont have it yet */
			int		c = 0;
			do {
				sqe = io_uring_get_sqe(ring);
				if (sqe)
					break;
				/*
				 * mostly debug code: had terrible
				 * experiences here...
				 */
				sched_yield();
				ret = io_uring_submit(ring);
				if (ret <= 0) {
					if (ret == -EAGAIN || ret == -EINTR)
						continue;
				}
				if (c++ > 10) {
					elog(INFO,
					     "unable to get new SQE, but io_uring_submit() got rc=%d(%s), io_uring_sq_space_left()=%d",
					     ret, strerror(-ret),
					     io_uring_sq_space_left(ring));
					c = 0;
				}
			} while (1);

			io_uring_prep_futex_wake(sqe, &futarr[i]->futex, 1,
						 FUTEX_BITSET_MATCH_ANY,
						 FUTEX2_SIZE_U32, 0);
			submitted++;
			/*
			 * FIXME: what to do if we have more than this?!
			 * batch it?
			 */
			Assert(submitted < IO_URING_QUEUE_DEPTH);
		}
	}

	if (submitted >= 0) {
		do {
			ret = io_uring_submit_and_get_events(ring);
			/* FIXME: fix ret != submitted ?! seems like bug?! */
			if (ret >= 0)
				break;
			else if (ret < 0 && (ret == -EAGAIN || ret == -EINTR))
				continue;
			else
				elog(INFO,
				     "failed vectored SQE submission, rc=%d(%s), io_uring_sq_space_left()=%d",
				     ret, strerror(-ret), io_uring_sq_space_left(ring));
		} while (1);
	}
}

static void	ReleaseSemaphores(int status, Datum arg);

/*
 * UringSemaphoreCreate
 *
 * Attempt to create a new unnamed semaphore. #endif
 */
static void
UringSemaphoreCreate(futex_sem_t * s)
{
	s->value = 1;
}


/*
 * UringSemaphoreKill	- removes a semaphore
 */
static void
UringSemaphoreKill(futex_sem_t * sem)
{
	/* it would be sem_close()/sem_destroy() here , but we dont have it */
	return;
}


/*
 * Report amount of shared memory needed for semaphores
 */
Size
PGSemaphoreShmemSize(int maxSemas)
{
	/* Need a PGSemaphoreData per semaphore */
	return mul_size(maxSemas, sizeof(PGSemaphoreData));
}

/*
 * PGReserveSemaphores --- initialize semaphore support
 *
 * This is called during postmaster start or shared memory reinitialization.
 * It should do whatever is needed to be able to support up to maxSemas
 * subsequent PGSemaphoreCreate calls.	Also, if any system resources are
 * acquired here or in PGSemaphoreCreate, register an on_shmem_exit callback
 * to release them.
 *
 * In the Posix implementation, we acquire semaphores on-demand; the maxSemas
 * parameter is just used to size the arrays.
 *
 * For unnamed semaphores, there is an array of PGSemaphoreData structs in
 * shared memory. For named semaphores, we keep a postmaster-local array of
 * sem_t pointers, which we use for releasing the semaphores when done. (This
 * design minimizes the dependency of postmaster shutdown on the contents of
 * shared memory, which a failed backend might have clobbered. We can't do
 * much about the possibility of sem_destroy() crashing, but we don't have to
 * expose the counters to other processes.)
 */
void
PGReserveSemaphores(int maxSemas)
{
	elog(WARNING, "using URING futex as semaphore APIs");

	/*
	 * We must use ShmemAllocUnlocked(), since the spinlock protecting
	 * ShmemAlloc() won't be ready yet.
	 */
	sharedSemas = (PGSemaphore)
		ShmemAllocUnlocked(PGSemaphoreShmemSize(maxSemas));

	numSems = 0;
	maxSems = maxSemas;

	on_shmem_exit(ReleaseSemaphores, 0);
}

/*
 * Release semaphores at shutdown or shmem reinitialization
 *
 * (called as an on_shmem_exit callback, hence funny argument list)
 */
static void
ReleaseSemaphores(int status, Datum arg)
{
	int		i;
	PGSemaphore	sema;

	for (i = 0; i < numSems; i++) {
		sema = &sharedSemas[i];
		UringSemaphoreKill(FUTEX_SEM_REF(sema));
	}

	if (ring_initialized == true) {
		io_uring_queue_exit(&ring);
		ring_initialized = false;
	}
}

void
PGSemaphoreInitializeLocal(void)
{
	/*
	 * We just intiialize one private io_uring structure
	 */
	/* FIXME: magic value -- align to something like max_locks_per_XXX ? */
#define IO_URING_QUEUE_DEPTH 32
	if (ring_initialized == false) {
		if (io_uring_queue_init(IO_URING_QUEUE_DEPTH, &ring, 0))
			elog(FATAL, "unable to initialize io_uring for futexes: %m");
		ring_initialized = true;
	}
}

/*
 * PGSemaphoreCreate
 *
 * Allocate a PGSemaphore structure with initial count 1
 */
PGSemaphore
PGSemaphoreCreate(void)
{
	PGSemaphore	sema;
	futex_sem_t    *newsem;

	/* Can't do this in a backend, because static state is postmaster's */
	Assert(!IsUnderPostmaster);

	if (numSems >= maxSems)
		elog(PANIC, "too many semaphores created");

	sema = &sharedSemas[numSems];
	newsem = FUTEX_SEM_REF(sema);
	UringSemaphoreCreate(newsem);
	numSems++;

	return sema;
}

/*
 * PGSemaphoreReset
 *
 * Reset a previously-initialized PGSemaphore to have count 0
 */
void
PGSemaphoreReset(PGSemaphore sema)
{
	int		one = 1;
	futex_sem_t    *s = FUTEX_SEM_REF(sema);
	__atomic_store(&s->value, &one, __ATOMIC_RELAXED);
}

/*
 * PGSemaphoreLock
 *
 * Lock a semaphore (decrement count), blocking if count would be < 0
 */
void
PGSemaphoreLock(PGSemaphore sema)
{
	/* simple system call, no io_uring here */
	fwait(FUTEX_SEM_REF(sema));
}

/*
 * PGSemaphoreUnlock
 *
 * Unlock a semaphore (increment count)
 */
void
PGSemaphoreUnlock(PGSemaphore sema)
{
	/* simple system call, no io_uring here */
	fpost(FUTEX_SEM_REF(sema));
}

/*
 * PGSemaphoreUnlockV
 *
 * Unlock a semaphore (increment count)
 */
void
PGSemaphoreUnlockV(PGSemaphore sema[], int n)
{
	futex_sem_t   **farr = malloc(n * sizeof(futex_sem_t));
	for (int i = 0; i < n; i++) {
		farr[i] = FUTEX_SEM_REF(sema[i]);
		/*
		 * one could also try with 1 futex/1 syscall instead of
		 * vectoring
		 */
		/* fpost_uring(&ring, farr[i]); */
		/* fpost(farr[i]); */
	}
	fpost_uring_many(&ring, farr, n);
	free(farr);
}

/*
 * PGSemaphoreTryLock
 *
 * Lock a semaphore only if able to do so without blocking
 */
bool
PGSemaphoreTryLock(PGSemaphore sema)
{
	futex_sem_t    *s = FUTEX_SEM_REF(sema);
	int		value = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
	while (1) {
		if (!value)
			return false;	/* False: failed to lock it */
		if (__atomic_compare_exchange_n
		    (&s->value, &value, value - 1, true, __ATOMIC_ACQUIRE,
		     __ATOMIC_RELAXED))
			return true;	/* True: managed to lock it */
	}
}
