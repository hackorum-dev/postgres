/*------------------------------------------------------------------------
 *
 * geqo_random.c
 *	   random number generator
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_random.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/geqo_random.h"


void
geqo_set_seed(PlannerInfo *root, double seed)
{
	GeqoPrivateData *private_data = (GeqoPrivateData *) root->join_search_private;

	/*
	 * XXX. This seeding algorithm could certainly be improved - but it is not
	 * critical to do so.
	 */
	memset(private_data->random_state, 0, sizeof(private_data->random_state));
	memcpy(private_data->random_state,
		   &seed,
		   Min(sizeof(private_data->random_state), sizeof(seed)));
}

double
geqo_rand(PlannerInfo *root)
{
	GeqoPrivateData *private_data = (GeqoPrivateData *) root->join_search_private;

	return pg_erand48(private_data->random_state);
}
