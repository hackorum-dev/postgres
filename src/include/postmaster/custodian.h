/*-------------------------------------------------------------------------
 *
 * custodian.h
 *   Exports from postmaster/custodian.c.
 *
 * Copyright (c) 2022, PostgreSQL Global Development Group
 *
 * src/include/postmaster/custodian.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _CUSTODIAN_H
#define _CUSTODIAN_H

/*
 * If you add a new task here, be sure to add its corresponding function
 * pointers to cust_task_functions in custodian.c.
 */
typedef enum CustodianTask
{
	FAKE_TASK,						/* placeholder until we have a real task */

	NUM_CUSTODIAN_TASKS,			/* new tasks go above */
	INVALID_CUSTODIAN_TASK
} CustodianTask;

extern void CustodianMain(void) pg_attribute_noreturn();
extern Size CustodianShmemSize(void);
extern void CustodianShmemInit(void);
extern void RequestCustodian(CustodianTask task, Datum arg);

#endif						/* _CUSTODIAN_H */
