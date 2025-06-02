#include "storage/rwoptspin.h"

#if PG_RWOPTSPIN_NATIVE

#include "storage/s_lock.h"
void
RWOptSpinAcquire_slowpath(RWOptSpin *spin, const char *file, int line, const char *func)
{
	SpinDelayStatus delay;

	init_spin_delay(&delay, file, line, func);
	do
	{
		perform_spin_delay(&delay);
	} while ((pg_atomic_read_u64(spin) & 1) != 0 ||
			 ((pg_atomic_fetch_or_u64(spin, 1) & 1) != 0));
	finish_spin_delay(&delay);
}

void
RWOptSpinRead_wait(RWOptSpin *spin, uint64 *version, const char *file, int line, const char *func)
{
	SpinDelayStatus delay;

	init_spin_delay(&delay, file, line, func);
	do
	{
		perform_spin_delay(&delay);
		*version = pg_atomic_read_u64(spin);
	} while (*version & 1);
	finish_spin_delay(&delay);
}
#endif
