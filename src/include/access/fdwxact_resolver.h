/*-------------------------------------------------------------------------
 *
 * fdwxact_resolver.h
 *	  PostgreSQL foreign transaction resolver definitions
 *
 *
 * Portions Copyright (c) 2018, PostgreSQL Global Development Group
 *
 * src/include/access/fdwxact_resolver.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FDWXACT_RESOLVER_H
#define FDWXACT_RESOLVER_H

#include "access/fdwxact.h"

extern void FdwXactRslvMain(Datum main_arg);
extern Size FdwXactRslvShmemSize(void);
extern void FdwXactRslvShmemInit(void);

extern void fdwxact_resolver_attach(int slot);
extern void fdwxact_maybe_launch_resolver(void);

extern int foreign_xact_resolver_timeout;

#endif		/* FDWXACT_RESOLVER_H */
