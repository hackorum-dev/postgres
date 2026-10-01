/*-------------------------------------------------------------------------
 *
 * spin.h
 *	   API for spinlocks.
 *
 *
 *	The interface to spinlocks is defined by the typedef "slock_t" and
 *	these functions:
 *
 *	void SpinLockInit(volatile slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	void SpinLockAcquire(volatile slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *
 *	void SpinLockRelease(volatile slock_t *lock)
 *		Unlock a previously acquired lock.
 *
 *	Load and store operations in calling code are guaranteed not to be
 *	reordered with respect to these operations, because they include a
 *	compiler barrier.  (Before PostgreSQL 9.5, callers needed to use a
 *	volatile qualifier to access data protected by spinlocks.)
 *
 *	Keep in mind the coding rule that spinlocks must not be held for more
 *	than a few instructions.  In particular, we assume it is not possible
 *	for a CHECK_FOR_INTERRUPTS() to occur while holding a spinlock, and so
 *	it is not necessary to do HOLD/RESUME_INTERRUPTS() in these functions.
 *
 *	These functions are implemented in terms of hardware-dependent macros
 *	supplied by s_lock.h.  There is not currently any extra functionality
 *	added by this header, but there has been in the past and may someday
 *	be again.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/spin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
#define SPIN_H

#include "storage/s_lock.h"
#include "port/atomics.h"
#include "c.h"

static inline void
SpinLockInit(volatile slock_t *lock)
{
	S_INIT_LOCK(lock);
}

static inline void
SpinLockAcquire(volatile slock_t *lock)
{
	S_LOCK(lock);
}

static inline void
SpinLockRelease(volatile slock_t *lock)
{
	S_UNLOCK(lock);
}


/*
 * Load/store a field also guarded by *lock. when the platform can access *p
 * with the correct width atomically, the lock is not used.
 * On platforms with PG_HAVE_ATOMIC_U64_SIMULATION, 64-bit accesses use *lock.
 */

#define SLOCK_DEFINE_SCALAR_IMPL(type, typename) \
static inline typename \
slock_read_##type##_impl(volatile slock_t *lock, volatile typename *p) \
{ \
	typename	val; \
\
	(void) lock; \
	AssertPointerAlignment(p, alignof(typename)); \
	__atomic_load(p, &val, __ATOMIC_RELAXED); \
	pg_read_barrier(); \
	return val; \
} \
static inline void \
slock_write_##type##_impl(volatile slock_t *lock, volatile typename *p, typename v) \
{ \
	(void) lock; \
	AssertPointerAlignment(p, alignof(typename)); \
	__atomic_store(p, &v, __ATOMIC_RELAXED); \
	pg_write_barrier(); \
}

#define SLOCK_DEFINE_LOCKED_IMPL(type, typename) \
static inline typename \
slock_read_##type##_impl(volatile slock_t *lock, volatile typename *p) \
{ \
	typename	val; \
\
	SpinLockAcquire(lock); \
	__atomic_load(p, &val, __ATOMIC_RELAXED); \
	SpinLockRelease(lock); \
	return val; \
} \
static inline void \
slock_write_##type##_impl(volatile slock_t *lock, volatile typename *p, typename v) \
{ \
	SpinLockAcquire(lock); \
	__atomic_store(p, &v, __ATOMIC_RELAXED); \
	SpinLockRelease(lock); \
}

#define SLOCK_SCALAR_READ(type, typename, lock, p) \
	(\
		StaticAssertExpr(sizeof(*(p)) == sizeof(typename), "slock_read_" #type " size mismatch"), \
		StaticAssertExpr(sizeof(v) == sizeof(typename), "slock_read_" #type " size mismatch"), \
	 slock_read_##type##_impl((lock), (volatile typename *) (p)))

#define SLOCK_SCALAR_WRITE(type, typename, lock, p, v) \
	((void) ( \
		StaticAssertExpr(sizeof(*(p)) == sizeof(typename), "slock_write_" #type " size mismatch"), \
		StaticAssertExpr(sizeof((v)) == sizeof(typename), "slock_write_" #type " size mismatch"), \
	 slock_write_##type##_impl((lock), (volatile typename *) (p), (typename) (v))))

SLOCK_DEFINE_SCALAR_IMPL(u8, unsigned char)
SLOCK_DEFINE_SCALAR_IMPL(u16, unsigned short)
SLOCK_DEFINE_SCALAR_IMPL(u32, unsigned int)
SLOCK_DEFINE_SCALAR_IMPL(ptr, Pointer)

#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
SLOCK_DEFINE_SCALAR_IMPL(u64, uint64)
#else							/* PG_HAVE_ATOMIC_U64_SIMULATION */
SLOCK_DEFINE_LOCKED_IMPL(u64, uint64)
#endif							/* PG_HAVE_ATOMIC_U64_SIMULATION */

#define slock_read_u8(lock, p) \
	SLOCK_SCALAR_READ(u8, uint8, lock, p)
#define slock_write_u8(lock, p, v) \
	SLOCK_SCALAR_WRITE(u8, uint8, lock, p, v)
#define slock_read_u16(lock, p) \
	SLOCK_SCALAR_READ(u16, uint16, lock, p)
#define slock_write_u16(lock, p, v) \
	SLOCK_SCALAR_WRITE(u16, uint16, lock, p, v)
#define slock_read_u32(lock, p) \
	SLOCK_SCALAR_READ(u32, uint32, lock, p)
#define slock_write_u32(lock, p, v) \
	SLOCK_SCALAR_WRITE(u32, uint32, lock, p, v)
#define slock_read_u64(lock, p) \
	SLOCK_SCALAR_READ(u64, uint64, lock, p)
#define slock_write_u64(lock, p, v) \
	SLOCK_SCALAR_WRITE(u64, uint64, lock, p, v)
#define slock_read_ptr(lock, p) \
	SLOCK_SCALAR_READ(ptr, Pointer, lock, p)
#define slock_write_ptr(lock, p, v) \
	SLOCK_SCALAR_WRITE(ptr, Pointer, lock, p, v)

#undef SLOCK_DEFINE_SCALAR_IMPL
#undef SLOCK_DEFINE_LOCKED_IMPL
#endif							/* SPIN_H */
