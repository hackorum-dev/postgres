/*-------------------------------------------------------------------------
 *
 * logicalworker.h
 *	  Exports for logical replication workers.
 *
 * Portions Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALWORKER_H
#define LOGICALWORKER_H

#include <signal.h>

/*
 * The default for max_logical_rep_clock_skew is -1, which means ignore clock
 * skew (the check is turned off).
 */
#define LR_CLOCK_SKEW_DEFAULT -1

/*
 * Worker Clock Skew Action.
 */
typedef enum
{
	LR_CLOCK_SKEW_ACTION_ERROR,
	LR_CLOCK_SKEW_ACTION_WAIT,
} LogicalRepClockSkewAction;

extern PGDLLIMPORT volatile sig_atomic_t ParallelApplyMessagePending;
extern PGDLLIMPORT int max_logical_rep_clock_skew;
extern PGDLLIMPORT int max_logical_rep_clock_skew_action;
extern PGDLLIMPORT int max_logical_rep_clock_skew_wait;

extern void ApplyWorkerMain(Datum main_arg);
extern void ParallelApplyWorkerMain(Datum main_arg);
extern void TablesyncWorkerMain(Datum main_arg);

extern bool IsLogicalWorker(void);
extern bool IsLogicalParallelApplyWorker(void);

extern void HandleParallelApplyMessageInterrupt(void);
extern void HandleParallelApplyMessages(void);

extern void LogicalRepWorkersWakeupAtCommit(Oid subid);

extern void AtEOXact_LogicalRepWorkers(bool isCommit);

#endif							/* LOGICALWORKER_H */
