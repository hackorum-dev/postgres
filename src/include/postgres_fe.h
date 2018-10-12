/*-------------------------------------------------------------------------
 *
 * postgres_fe.h
 *	  Primary include file for PostgreSQL client-side .c files
 *
 * This should be the first file included by PostgreSQL client libraries and
 * application programs --- but not by backend modules, which should include
 * postgres.h.
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/postgres_fe.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRES_FE_H
#define POSTGRES_FE_H

#ifndef FRONTEND
#define FRONTEND 1
#endif

#include "c.h"

#include "common/fe_memutils.h"

/*
 * Limit on the length of passwords we will try to process.  Note that this does
 * not restrict the creation of longer passwords via commands such as CREATE
 * ROLE and ALTER ROLE.  It merely restricts the length of passwords accepted by
 * client utility prompts (e.g. psql).
 *
 * One byte is reserved at the end for the '\0' byte, so the user-facing maximum
 * length is actually 99.
 */
#define PROMPT_MAX_PASSWORD_LENGTH	100

#endif							/* POSTGRES_FE_H */
