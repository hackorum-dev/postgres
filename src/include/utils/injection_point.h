/*-------------------------------------------------------------------------
 * injection_point.h
 *	  Definitions related to injection points.
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * src/include/utils/injection_point.h
 *-------------------------------------------------------------------------
 */

#ifndef INJECTION_POINT_H
#define INJECTION_POINT_H

/*
 * Injections points require --enable-injection-points.
 */
#ifdef USE_INJECTION_POINTS
#define INJECTION_POINT(name) InjectionPointRun(name)
#define INJECTION_POINT_1ARG(name, arg1) InjectionPointRun1Arg(name, arg1)
#else
#define INJECTION_POINT(name) ((void) name)
#define INJECTION_POINT_1ARG(name) ((void) name, (void) arg1)
#endif

/*
 * Typedef for callback function launched by an injection point.
 */
typedef void (*InjectionPointCallback) (const char *name);
typedef void (*InjectionPointCallback1Arg) (const char *name, void *arg1);

extern Size InjectionPointShmemSize(void);
extern void InjectionPointShmemInit(void);

extern void InjectionPointAttach(const char *name,
								 const char *library,
								 const char *function,
								 int num_args);
extern void InjectionPointRun(const char *name);
extern void InjectionPointRun1Arg(const char *name, void *arg1);
extern void InjectionPointDetach(const char *name);

#endif							/* INJECTION_POINT_H */
